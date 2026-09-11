/* SPDX-License-Identifier: MIT */
/*
 * EGL for Haiku, backed by the IntelGfx Iris Gallium screen.
 *
 * Mesa has shipped a Haiku EGL driver since 2014 whose Initialize hook is
 * `#if 1 /* Does not work. *\/ return EGL_FALSE;`, so eglInitialize has always
 * failed with EGL_NOT_INITIALIZED and no EGL client has ever run on Haiku.
 * The old code also had no notion of a renderer: window surfaces created a
 * BGLView, which builds a second, unrelated GL context through the Haiku
 * OpenGL roster, and contexts themselves were empty shells.
 *
 * This implementation drops that shell and drives the same Gallium stack the
 * "Intel Gallium" OpenGL add-on uses: intel_haiku_screen_create() opens
 * /dev/graphics/intel_extreme_* through the IntelGfx ABI and builds an Iris
 * pipe_screen, and hgl_create_display() wraps it in the Mesa state tracker
 * manager. EGL contexts are st_context_iface objects on that screen, so they
 * are hardware contexts on the Intel GPU.
 *
 * Surfaceless contexts (EGL_KHR_surfaceless_context) are the important case:
 * Firefox's WebGL and its glxtest probe both bind a context with EGL_NO_SURFACE
 * and render into FBOs they create themselves. Mesa's st_api_make_current
 * accepts NULL framebuffers and binds an incomplete framebuffer instead, so
 * this is a first-class configuration rather than a workaround.
 *
 * Pbuffer and window surfaces are implemented too. Presentation is a CPU
 * readback of the Gallium colour buffer into a BBitmap that is drawn into a
 * BView the surface owns, which is the same path the BGLView renderer takes --
 * the Iris winsys has no scanout of its own here, so only the final copy
 * crosses to the app_server while all rendering stays on the GPU. Window
 * surfaces matter beyond their own use: eglChooseConfig defaults
 * EGL_SURFACE_TYPE to EGL_WINDOW_BIT, so a driver without them matches no
 * configs for callers that never mention a surface type.
 *
 * This is Intel-specific by construction: the screen comes from IntelGfx. A
 * driver-independent Haiku EGL would need the same treatment for whatever
 * other pipe_screen sources appear, software rendering included.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "eglarray.h"
#include "eglconfig.h"
#include "eglcontext.h"
#include "eglcurrent.h"
#include "egldevice.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "eglimage.h"
#include "egllog.h"
#include "eglsurface.h"
#include "egltypedefs.h"

#include "glapi/glapi.h"

#include "hgl_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "state_tracker/st_context.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include <Bitmap.h>
#include <View.h>
#include <Window.h>

#include "IntelScreen.h"

#define ERROR(x...) _eglLog(_EGL_WARNING, "egl_haiku: " x)
#define INFO(x...) _eglLog(_EGL_INFO, "egl_haiku: " x)


/* GTK's Wayland backend hands eglCreateWindowSurface a wl_egl_window, not a
 * BWindow.  Keep the small stable version-3 ABI here instead of adding a
 * build-time dependency on Wayland: IntelGfx is built with Haiku's base build
 * packages, while Wayland is an optional runtime package.  All Wayland entry
 * points are consequently resolved only when this native-window type appears.
 *
 * This layout is the public wayland-egl-backend ABI.  Its version field is
 * deliberately the old surface slot, making version 3 distinguishable from a
 * C++ BWindow vtable without dereferencing the latter as the wrong class. */
struct wl_proxy;
struct wl_display;
struct wl_event_queue;
struct wl_registry;
struct wl_shm;
struct wl_shm_pool;
struct wl_buffer;
struct wl_surface;

struct wl_interface {
	const char*	name;
	int		version;
	int		method_count;
	const void*	methods;
	int		event_count;
	const void*	events;
};

struct wl_egl_window {
	const intptr_t	version;
	int		width;
	int		height;
	int		dx;
	int		dy;
	int		attached_width;
	int		attached_height;
	void*		driver_private;
	void		(*resize_callback)(struct wl_egl_window*, void*);
	void		(*destroy_window_callback)(void*);
	struct wl_surface* surface;
};

enum {
	WL_EGL_WINDOW_VERSION = 3,
	WL_MARSHAL_FLAG_DESTROY = 1,
	WL_DISPLAY_SYNC = 0,
	WL_DISPLAY_GET_REGISTRY = 1,
	WL_REGISTRY_BIND = 0,
	WL_SHM_CREATE_POOL = 0,
	WL_SHM_POOL_CREATE_BUFFER = 0,
	WL_SHM_POOL_DESTROY = 1,
	WL_BUFFER_DESTROY = 0,
	WL_SURFACE_ATTACH = 1,
	WL_SURFACE_DAMAGE = 2,
	WL_SURFACE_COMMIT = 6,
	WL_SURFACE_DAMAGE_BUFFER = 9,
	WL_SHM_FORMAT_ARGB8888 = 0,
	WL_SHM_FORMAT_XRGB8888 = 1,
	HAIKU_WL_BUFFER_COUNT = 3,
};

struct haiku_wayland_api {
	void*	lib;
	wl_proxy* (*marshal_flags)(wl_proxy*, uint32_t, const wl_interface*,
		uint32_t, uint32_t, ...);
	int (*add_listener)(wl_proxy*, void (**)(void), void*);
	const char* (*get_class)(wl_proxy*);
	uint32_t (*get_version)(wl_proxy*);
	void (*proxy_destroy)(wl_proxy*);
	wl_proxy* (*create_wrapper)(void*);
	void (*wrapper_destroy)(void*);
	void (*set_queue)(wl_proxy*, wl_event_queue*);
	wl_event_queue* (*create_queue)(wl_display*);
	void (*destroy_queue)(wl_event_queue*);
	int (*dispatch_queue_pending)(wl_display*, wl_event_queue*);
	int (*roundtrip_queue)(wl_display*, wl_event_queue*);
	int (*display_flush)(wl_display*);

	const wl_interface* registry_interface;
	const wl_interface* shm_interface;
	const wl_interface* shm_pool_interface;
	const wl_interface* buffer_interface;
};

struct haiku_wayland_buffer {
	wl_buffer*		proxy;
	uint8_t*		data;
	bool			busy;
};


struct haiku_egl_display {
	struct hgl_display*	display;
	struct pipe_screen*	screen;

	/* Serialises every state-tracker call made through this display.
	 *
	 * Gallium does not tolerate concurrent use of one pipe context: as
	 * GalliumContext::Draw puts it, two threads inside one batch flush corrupt
	 * its buffer. The BGLView renderer guards CreateContext, SetCurrentContext,
	 * SwapBuffers and Draw with its own mutex for exactly that reason, and EGL
	 * needs the same protection -- more so, because a browser drives it from a
	 * compositor thread and a canvas thread at once. hgl_display carries a
	 * mutex of its own but hgl_create_display never initialises it, so this one
	 * is ours. Recursive because MakeCurrent unbinds the previous context and
	 * surfaces through the same entry points that take this lock. */
	mtx_t			lock;

	/* Created lazily, because native BWindow users need no Wayland runtime. */
	haiku_wayland_api	wl;
	wl_display*		wayland_display;
	wl_event_queue*	wl_queue;
	wl_registry*		wayland_registry;
	wl_shm*		wayland_shm;
};

struct haiku_egl_context {
	_EGLContext		base;
	struct st_context_iface* st;

	/* hgl_st_framebuffer_validate casts st_context_iface::st_manager_private
	 * straight to an hgl_context and reads the drawable size out of it, so
	 * that pointer has to be a real one. It carries the visual as well; the
	 * size is refreshed from the bound draw surface in MakeCurrent. */
	struct hgl_context	hgl;
};

struct haiku_egl_surface {
	_EGLSurface		base;
	/* hgl_st_framebuffer_validate reads the requested size out of an
	 * hgl_context, so every surface carries one. It is a size carrier, not a
	 * GL context. */
	struct hgl_context*	shim;
	struct hgl_buffer*	buffer;

	/* Window surfaces only. The view is ours: we add it to the caller's
	 * window and blit finished frames into it. */
	BWindow*		window;
	BView*			view;
	BBitmap*		bitmap;

	/* Wayland window surfaces. Rendering is read back into one of these shm
	 * buffers, then attached to the real wl_surface held by wl_egl_window. */
	bool			wl_native;
	wl_egl_window*		wl_window;
	haiku_wayland_buffer	wl_buffers[HAIKU_WL_BUFFER_COUNT];
	void*			wl_map;
	size_t			wl_map_size;
	uint32_t		wl_stride;
	uint32_t		wl_width;
	uint32_t		wl_height;
	unsigned		wl_next_buffer;
};

struct haiku_egl_config {
	_EGLConfig		base;
	enum pipe_format	color_format;
	enum pipe_format	depth_stencil_format;
	bool			double_buffered;
};

_EGL_DRIVER_TYPECAST(haiku_egl_display, _EGLDisplay, obj->DriverData)
_EGL_DRIVER_TYPECAST(haiku_egl_context, _EGLContext, obj)
_EGL_DRIVER_TYPECAST(haiku_egl_surface, _EGLSurface, obj)
_EGL_DRIVER_TYPECAST(haiku_egl_config, _EGLConfig, obj)


#ifndef INTELGFX_EGL_LOCKING
#	define INTELGFX_EGL_LOCKING 1
#endif

/* Holds haiku_egl_display::lock for the duration of a driver entry point. */
class DisplayLock {
public:
	explicit DisplayLock(struct haiku_egl_display* display)
		:
		fDisplay(display)
	{
#if INTELGFX_EGL_LOCKING
		if (fDisplay != NULL)
			mtx_lock(&fDisplay->lock);
#endif
	}

	~DisplayLock()
	{
#if INTELGFX_EGL_LOCKING
		if (fDisplay != NULL)
			mtx_unlock(&fDisplay->lock);
#endif
	}

private:
	DisplayLock(const DisplayLock&);
	DisplayLock& operator=(const DisplayLock&);

	struct haiku_egl_display* fDisplay;
};


/* Every config we publish. Firefox asks for 8/8/8/8 with and without a depth
 * and stencil buffer; the rest are here so ordinary eglChooseConfig filters
 * behave. */
struct config_template {
	bool		alpha;
	unsigned	depth;
	unsigned	stencil;
	bool		double_buffered;
};

static const struct config_template kConfigTemplates[] = {
	{ true,  0,  0, false }, { true,  0,  0, true },
	{ true, 24,  8, false }, { true, 24,  8, true },
	{ true, 16,  0, false }, { true, 16,  0, true },
	{ false, 0,  0, false }, { false, 0,  0, true },
	{ false, 24, 8, false }, { false, 24, 8, true },
};


struct wl_registry_listener {
	void (*global)(void*, wl_registry*, uint32_t, const char*, uint32_t);
	void (*global_remove)(void*, wl_registry*, uint32_t);
};

struct wl_buffer_listener {
	void (*release)(void*, wl_buffer*);
};


template<typename T>
static bool
load_wayland_symbol(void* library, const char* name, T& value)
{
	value = reinterpret_cast<T>(dlsym(library, name));
	if (value == NULL) {
		ERROR("Wayland runtime has no %s\n", name);
		return false;
	}
	return true;
}


static void
wayland_registry_global(void* data, wl_registry* registry, uint32_t name,
	const char* interface, uint32_t version)
{
	struct haiku_egl_display* hdisp = (struct haiku_egl_display*)data;
	if (strcmp(interface, "wl_shm") != 0 || hdisp->wayland_shm != NULL)
		return;

	uint32_t bind_version = version < 1 ? version : 1;
	hdisp->wayland_shm = (wl_shm*)hdisp->wl.marshal_flags((wl_proxy*)registry,
		WL_REGISTRY_BIND, hdisp->wl.shm_interface, bind_version, 0, name,
		hdisp->wl.shm_interface->name, bind_version, NULL);
}


static void
wayland_registry_global_remove(void*, wl_registry*, uint32_t)
{
}


static const wl_registry_listener kWaylandRegistryListener = {
	wayland_registry_global,
	wayland_registry_global_remove,
};


static void
destroy_wayland_display(struct haiku_egl_display* hdisp)
{
	if (hdisp->wl.proxy_destroy != NULL) {
		if (hdisp->wayland_shm != NULL)
			hdisp->wl.proxy_destroy((wl_proxy*)hdisp->wayland_shm);
		if (hdisp->wayland_registry != NULL)
			hdisp->wl.proxy_destroy((wl_proxy*)hdisp->wayland_registry);
	}
	if (hdisp->wl.destroy_queue != NULL && hdisp->wl_queue != NULL)
		hdisp->wl.destroy_queue(hdisp->wl_queue);
	if (hdisp->wl.lib != NULL)
		dlclose(hdisp->wl.lib);

	memset(&hdisp->wl, 0, sizeof(hdisp->wl));
	hdisp->wayland_display = NULL;
	hdisp->wl_queue = NULL;
	hdisp->wayland_registry = NULL;
	hdisp->wayland_shm = NULL;
}


static bool
init_wayland_display(struct haiku_egl_display* hdisp, void* native_display)
{
	wl_proxy* display_wrapper = NULL;

	if (hdisp->wayland_display != NULL)
		return hdisp->wayland_display == (wl_display*)native_display;
	if (native_display == NULL) {
		ERROR("wl_egl_window used with EGL_DEFAULT_DISPLAY\n");
		return false;
	}

	hdisp->wl.lib = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
	if (hdisp->wl.lib == NULL)
		hdisp->wl.lib = dlopen("libwayland-client.so", RTLD_LAZY | RTLD_LOCAL);
	if (hdisp->wl.lib == NULL) {
		ERROR("could not load the Wayland client runtime: %s\n", dlerror());
		return false;
	}

#define LOAD_WL_FUNCTION(field, symbol) \
	if (!load_wayland_symbol(hdisp->wl.lib, symbol, hdisp->wl.field)) \
		goto fail
	LOAD_WL_FUNCTION(marshal_flags, "wl_proxy_marshal_flags");
	LOAD_WL_FUNCTION(add_listener, "wl_proxy_add_listener");
	LOAD_WL_FUNCTION(get_class, "wl_proxy_get_class");
	LOAD_WL_FUNCTION(get_version, "wl_proxy_get_version");
	LOAD_WL_FUNCTION(proxy_destroy, "wl_proxy_destroy");
	LOAD_WL_FUNCTION(create_wrapper, "wl_proxy_create_wrapper");
	LOAD_WL_FUNCTION(wrapper_destroy, "wl_proxy_wrapper_destroy");
	LOAD_WL_FUNCTION(set_queue, "wl_proxy_set_queue");
	LOAD_WL_FUNCTION(create_queue, "wl_display_create_queue");
	LOAD_WL_FUNCTION(destroy_queue, "wl_event_queue_destroy");
	LOAD_WL_FUNCTION(dispatch_queue_pending, "wl_display_dispatch_queue_pending");
	LOAD_WL_FUNCTION(roundtrip_queue, "wl_display_roundtrip_queue");
	LOAD_WL_FUNCTION(display_flush, "wl_display_flush");
#undef LOAD_WL_FUNCTION

	if (!load_wayland_symbol(hdisp->wl.lib, "wl_registry_interface",
			hdisp->wl.registry_interface)
		|| !load_wayland_symbol(hdisp->wl.lib, "wl_shm_interface",
			hdisp->wl.shm_interface)
		|| !load_wayland_symbol(hdisp->wl.lib, "wl_shm_pool_interface",
			hdisp->wl.shm_pool_interface)
		|| !load_wayland_symbol(hdisp->wl.lib, "wl_buffer_interface",
			hdisp->wl.buffer_interface))
		goto fail;

	hdisp->wayland_display = (wl_display*)native_display;
	hdisp->wl_queue = hdisp->wl.create_queue(hdisp->wayland_display);
	if (hdisp->wl_queue == NULL)
		goto fail;

	/* A wrapper makes the registry and every child object inherit our queue.
	 * Waiting for shm-buffer release events therefore never dispatches GTK's
	 * input/window events on WebRender's render thread. */
	display_wrapper = hdisp->wl.create_wrapper(hdisp->wayland_display);
	if (display_wrapper == NULL)
		goto fail;
	hdisp->wl.set_queue(display_wrapper, hdisp->wl_queue);
	hdisp->wayland_registry = (wl_registry*)hdisp->wl.marshal_flags(display_wrapper,
		WL_DISPLAY_GET_REGISTRY, hdisp->wl.registry_interface, 1, 0, NULL);
	hdisp->wl.wrapper_destroy(display_wrapper);
	if (hdisp->wayland_registry == NULL)
		goto fail;

	if (hdisp->wl.add_listener((wl_proxy*)hdisp->wayland_registry,
			(void (**)(void))&kWaylandRegistryListener, hdisp) < 0)
		goto fail;
	if (hdisp->wl.roundtrip_queue(hdisp->wayland_display, hdisp->wl_queue) < 0
		|| hdisp->wayland_shm == NULL) {
		ERROR("Wayland compositor did not advertise wl_shm\n");
		goto fail;
	}

	INFO("connected EGL presentation to wl_shm\n");
	return true;

fail:
	destroy_wayland_display(hdisp);
	return false;
}


static enum pipe_format
depth_stencil_format(unsigned depth, unsigned stencil)
{
	if (depth == 0 && stencil == 0)
		return PIPE_FORMAT_NONE;
	if (depth == 16 && stencil == 0)
		return PIPE_FORMAT_Z16_UNORM;
	return PIPE_FORMAT_Z24_UNORM_S8_UINT;
}


static struct st_visual*
create_visual(const struct haiku_egl_config* conf)
{
	struct st_visual* visual = CALLOC_STRUCT(st_visual);
	if (!visual)
		return NULL;

	visual->color_format = conf->color_format;
	visual->depth_stencil_format = conf->depth_stencil_format;
	visual->accum_format = PIPE_FORMAT_NONE;
	/* 0, not 1: hgl_create_st_visual leaves this zero and Iris builds its ISL
	 * surfaces from it. */
	visual->samples = 0;
	visual->buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK;
	if (conf->double_buffered)
		visual->buffer_mask |= ST_ATTACHMENT_BACK_LEFT_MASK;
	if (conf->depth_stencil_format != PIPE_FORMAT_NONE)
		visual->buffer_mask |= ST_ATTACHMENT_DEPTH_STENCIL_MASK;

	return visual;
}


static bool
add_configs(_EGLDisplay* disp, struct pipe_screen* screen)
{
	int maxTexture = screen->get_param(screen, PIPE_CAP_MAX_TEXTURE_2D_SIZE);
	if (maxTexture <= 0)
		maxTexture = 8192;

	EGLint id = 1;
	for (unsigned i = 0; i < ARRAY_SIZE(kConfigTemplates); i++) {
		const struct config_template& tmpl = kConfigTemplates[i];

		enum pipe_format color = tmpl.alpha
			? PIPE_FORMAT_BGRA8888_UNORM : PIPE_FORMAT_BGRX8888_UNORM;
		enum pipe_format ds = depth_stencil_format(tmpl.depth, tmpl.stencil);

		if (!screen->is_format_supported(screen, color, PIPE_TEXTURE_2D, 0, 0,
				PIPE_BIND_RENDER_TARGET))
			continue;
		if (ds != PIPE_FORMAT_NONE
			&& !screen->is_format_supported(screen, ds, PIPE_TEXTURE_2D, 0, 0,
				PIPE_BIND_DEPTH_STENCIL))
			continue;

		struct haiku_egl_config* conf = CALLOC_STRUCT(haiku_egl_config);
		if (!conf)
			return false;

		_eglInitConfig(&conf->base, disp, id);

		conf->color_format = color;
		conf->depth_stencil_format = ds;
		conf->double_buffered = tmpl.double_buffered;

		conf->base.RedSize = 8;
		conf->base.GreenSize = 8;
		conf->base.BlueSize = 8;
		conf->base.AlphaSize = tmpl.alpha ? 8 : 0;
		conf->base.LuminanceSize = 0;
		conf->base.BufferSize = conf->base.RedSize + conf->base.GreenSize
			+ conf->base.BlueSize + conf->base.AlphaSize;
		conf->base.ColorBufferType = EGL_RGB_BUFFER;
		conf->base.DepthSize = tmpl.depth;
		conf->base.StencilSize = tmpl.stencil;
		conf->base.SampleBuffers = 0;
		conf->base.Samples = 0;
		conf->base.ConfigCaveat = EGL_NONE;
		conf->base.NativeRenderable = EGL_FALSE;
		conf->base.NativeVisualID = 0;
		conf->base.NativeVisualType = EGL_NONE;
		conf->base.TransparentType = EGL_NONE;
		conf->base.Level = 0;
		conf->base.BindToTextureRGB = EGL_FALSE;
		conf->base.BindToTextureRGBA = EGL_FALSE;
		conf->base.MinSwapInterval = 0;
		conf->base.MaxSwapInterval = 1;

		/* eglChooseConfig defaults EGL_SURFACE_TYPE to EGL_WINDOW_BIT, so a
		 * config that omits it matches nothing for callers that do not ask
		 * for a surface type -- Firefox's glxtest among them. Both bits are
		 * implemented. */
		conf->base.SurfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
		conf->base.MaxPbufferWidth = maxTexture;
		conf->base.MaxPbufferHeight = maxTexture;
		conf->base.MaxPbufferPixels = maxTexture * maxTexture;

		conf->base.RenderableType = disp->ClientAPIs;
		conf->base.Conformant = disp->ClientAPIs;

		if (!_eglValidateConfig(&conf->base, EGL_FALSE)) {
			ERROR("rejected generated config %d\n", id);
			free(conf);
			continue;
		}

		_eglLinkConfig(&conf->base);
		id++;
	}

	if (_eglGetArraySize(disp->Configs) == 0) {
		ERROR("no usable configs on this screen\n");
		return false;
	}

	return true;
}



extern "C" EGLBoolean
init_haiku(_EGLDisplay* disp)
{
	struct haiku_egl_display* hdisp = CALLOC_STRUCT(haiku_egl_display);
	if (!hdisp) {
		_eglError(EGL_BAD_ALLOC, "init_haiku");
		return EGL_FALSE;
	}

	mtx_init(&hdisp->lock, mtx_recursive);

	hdisp->screen = intel_haiku_screen_create();
	if (hdisp->screen == NULL) {
		/* No IntelGfx device, or the kernel ABI did not match. Failing here
		 * leaves eglInitialize reporting EGL_NOT_INITIALIZED, which is the
		 * correct answer: there is no other renderer we can reach. */
		ERROR("no IntelGfx render device available\n");
		mtx_destroy(&hdisp->lock);
		free(hdisp);
		_eglError(EGL_NOT_INITIALIZED, "init_haiku: no render device");
		return EGL_FALSE;
	}

	hdisp->display = hgl_create_display(hdisp->screen);
	if (hdisp->display == NULL) {
		ERROR("could not create the Gallium state tracker display\n");
		hdisp->screen->destroy(hdisp->screen);
		mtx_destroy(&hdisp->lock);
		free(hdisp);
		_eglError(EGL_NOT_INITIALIZED, "init_haiku: no state tracker");
		return EGL_FALSE;
	}

	disp->DriverData = hdisp;

	/* Deliberately no disp->Device. Without libdrm -- which Haiku has no use
	 * for, the IntelGfx ABI is not DRM -- _eglAddDevice can only hand back the
	 * built-in software device, and that device advertises
	 * EGL_MESA_device_software. Consumers read that as "this display is not
	 * accelerated": Firefox's glxtest, for one, records MESA_ACCELERATED=FALSE
	 * and then blocks hardware compositing, which would be exactly wrong for
	 * an Iris context on the GPU. Leaving the display without a device makes
	 * eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) fail, which callers handle, and
	 * they fall back to assuming acceleration.
	 */

	disp->ClientAPIs = EGL_OPENGL_BIT | EGL_OPENGL_ES_BIT
		| EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT;

	disp->Extensions.KHR_create_context = EGL_TRUE;
	disp->Extensions.KHR_no_config_context = EGL_TRUE;
	disp->Extensions.KHR_surfaceless_context = EGL_TRUE;
	disp->Extensions.KHR_context_flush_control = EGL_TRUE;
	/* Firefox's glxtest probe calls eglGetDisplayDriverName unconditionally,
	 * and Mesa's eglapi asserts the display advertises this before dispatching
	 * to the driver. */
	disp->Extensions.MESA_query_driver = EGL_TRUE;

	disp->Version = 14;

	if (!add_configs(disp, hdisp->screen)) {
		hgl_destroy_display(hdisp->display);
		hdisp->screen->destroy(hdisp->screen);
		free(hdisp);
		disp->DriverData = NULL;
		_eglError(EGL_NOT_INITIALIZED, "init_haiku: no configs");
		return EGL_FALSE;
	}

	INFO("initialized on %s\n", hdisp->screen->get_name(hdisp->screen));
	return EGL_TRUE;
}


extern "C" EGLBoolean
haiku_terminate(_EGLDisplay* disp)
{
	struct haiku_egl_display* hdisp = haiku_egl_display(disp);
	if (!hdisp)
		return EGL_TRUE;

	mtx_lock(&hdisp->lock);
	_eglReleaseDisplayResources(disp);
	destroy_wayland_display(hdisp);

	if (hdisp->display)
		hgl_destroy_display(hdisp->display);
	if (hdisp->screen)
		hdisp->screen->destroy(hdisp->screen);

	mtx_unlock(&hdisp->lock);
	mtx_destroy(&hdisp->lock);

	free(hdisp);
	disp->DriverData = NULL;
	return EGL_TRUE;
}


static enum st_profile_type
profile_for(const _EGLContext* ctx)
{
	switch (ctx->ClientAPI) {
		case EGL_OPENGL_ES_API:
			return ctx->ClientMajorVersion <= 1
				? ST_PROFILE_OPENGL_ES1 : ST_PROFILE_OPENGL_ES2;
		case EGL_OPENGL_API:
		default:
			/* Ask for core only when the caller asked for a core profile of
			 * 3.2 or newer; everything else gets compatibility, which is what
			 * EGL_OPENGL_API means without EGL_KHR_create_context hints. */
			if (ctx->Profile == EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR
				&& (ctx->ClientMajorVersion > 3
					|| (ctx->ClientMajorVersion == 3
						&& ctx->ClientMinorVersion >= 2)))
				return ST_PROFILE_OPENGL_CORE;
			return ST_PROFILE_DEFAULT;
	}
}


extern "C" _EGLContext*
haiku_create_context(_EGLDisplay* disp, _EGLConfig* conf,
	_EGLContext* share_list, const EGLint* attrib_list)
{
	struct haiku_egl_display* hdisp = haiku_egl_display(disp);
	DisplayLock guard(hdisp);
	struct haiku_egl_context* context = CALLOC_STRUCT(haiku_egl_context);
	if (!context) {
		_eglError(EGL_BAD_ALLOC, "haiku_create_context");
		return NULL;
	}

	if (!_eglInitContext(&context->base, disp, conf, attrib_list))
		goto cleanup;

	{
		struct st_context_attribs attribs;
		memset(&attribs, 0, sizeof(attribs));
		attribs.profile = profile_for(&context->base);
		attribs.major = context->base.ClientMajorVersion;
		attribs.minor = context->base.ClientMinorVersion;

		if (context->base.Flags & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR)
			attribs.flags |= ST_CONTEXT_FLAG_DEBUG;
		if (context->base.Flags & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR)
			attribs.flags |= ST_CONTEXT_FLAG_FORWARD_COMPATIBLE;
		if (context->base.Flags & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR)
			attribs.flags |= ST_CONTEXT_FLAG_ROBUST_ACCESS;

		/* EGL_KHR_no_config_context: a context created without a config still
		 * needs a visual to describe the framebuffers it may later be bound
		 * to, so fall back to a plain 8888 one. */
		if (conf != NULL) {
			struct haiku_egl_config* hconf = haiku_egl_config(conf);
			context->hgl.stVisual = create_visual(hconf);
		} else {
			struct haiku_egl_config fallback;
			memset(&fallback, 0, sizeof(fallback));
			fallback.color_format = PIPE_FORMAT_BGRA8888_UNORM;
			fallback.depth_stencil_format = PIPE_FORMAT_Z24_UNORM_S8_UINT;
			fallback.double_buffered = true;
			context->hgl.stVisual = create_visual(&fallback);
		}

		if (!context->hgl.stVisual) {
			_eglError(EGL_BAD_ALLOC, "haiku_create_context");
			goto cleanup;
		}
		attribs.visual = *context->hgl.stVisual;

		struct st_context_iface* shared = NULL;
		if (share_list != NULL)
			shared = haiku_egl_context(share_list)->st;

		enum st_context_error result = ST_CONTEXT_SUCCESS;
		context->st = hdisp->display->api->create_context(hdisp->display->api,
			hdisp->display->manager, &attribs, &result, shared);

		if (!context->st) {
			static const char* const kErrors[] = {
				"success", "no memory", "bad api", "bad version",
				"bad flag", "unknown attribute", "unknown flag",
			};
			const char* why = (unsigned)result < ARRAY_SIZE(kErrors)
				? kErrors[result] : "unknown";
			ERROR("state tracker refused the context: %s\n", why);
			_eglError(result == ST_CONTEXT_ERROR_NO_MEMORY
				? EGL_BAD_ALLOC : EGL_BAD_MATCH, "haiku_create_context");
			goto cleanup;
		}

		context->hgl.display = hdisp->display;
		context->hgl.st = context->st;
		context->st->st_manager_private = &context->hgl;
	}

	return &context->base;

cleanup:
	if (context->hgl.stVisual)
		free(context->hgl.stVisual);
	free(context);
	return NULL;
}


extern "C" EGLBoolean
haiku_destroy_context(_EGLDisplay* disp, _EGLContext* ctx)
{
	struct haiku_egl_display* hdisp = haiku_egl_display(disp);
	struct haiku_egl_context* context = haiku_egl_context(ctx);
	DisplayLock guard(hdisp);

	if (_eglPutContext(ctx)) {
		if (context->st)
			context->st->destroy(context->st);
		if (context->hgl.stVisual)
			free(context->hgl.stVisual);
		free(context);
	}
	return EGL_TRUE;
}


extern "C" _EGLSurface*
haiku_create_pbuffer_surface(_EGLDisplay* disp, _EGLConfig* conf,
	const EGLint* attrib_list)
{
	struct haiku_egl_display* hdisp = haiku_egl_display(disp);
	DisplayLock guard(hdisp);
	struct haiku_egl_surface* surface = CALLOC_STRUCT(haiku_egl_surface);
	if (!surface) {
		_eglError(EGL_BAD_ALLOC, "haiku_create_pbuffer_surface");
		return NULL;
	}

	if (!_eglInitSurface(&surface->base, disp, EGL_PBUFFER_BIT, conf,
			attrib_list, NULL)) {
		free(surface);
		return NULL;
	}

	surface->shim = CALLOC_STRUCT(hgl_context);
	if (!surface->shim) {
		_eglError(EGL_BAD_ALLOC, "haiku_create_pbuffer_surface");
		goto cleanup;
	}

	surface->shim->display = hdisp->display;
	surface->shim->stVisual = create_visual(haiku_egl_config(conf));
	if (!surface->shim->stVisual) {
		_eglError(EGL_BAD_ALLOC, "haiku_create_pbuffer_surface");
		goto cleanup;
	}
	surface->shim->width = surface->base.Width;
	surface->shim->height = surface->base.Height;

	surface->buffer = hgl_create_st_framebuffer(surface->shim, NULL);
	if (!surface->buffer) {
		ERROR("could not create a pbuffer framebuffer\n");
		_eglError(EGL_BAD_ALLOC, "haiku_create_pbuffer_surface");
		goto cleanup;
	}

	return &surface->base;

cleanup:
	if (surface->shim) {
		if (surface->shim->stVisual)
			free(surface->shim->stVisual);
		free(surface->shim);
	}
	free(surface);
	return NULL;
}


static void
wayland_buffer_release(void* data, wl_buffer*)
{
	((haiku_wayland_buffer*)data)->busy = false;
}


static const wl_buffer_listener kWaylandBufferListener = {
	wayland_buffer_release,
};


static int
create_shm_file()
{
	/* Haiku's in-process compositor maps the descriptor into a cloneable area.
	 * Unlink immediately: the descriptor and the server mapping own its
	 * lifetime, with no filesystem entry left behind after a crash. */
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	uint64_t seed = ((uint64_t)now.tv_sec << 32) ^ (uint64_t)now.tv_nsec
		^ (uint64_t)getpid();

	for (unsigned attempt = 0; attempt < 100; attempt++) {
		char name[48];
		snprintf(name, sizeof(name), "/intelgfx-egl-%" PRIx64,
			seed + attempt * UINT64_C(0x9e3779b97f4a7c15));
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
		if (errno != EEXIST)
			break;
	}
	return -1;
}


static void
destroy_wayland_buffers(struct haiku_egl_surface* surface,
	struct haiku_egl_display* hdisp)
{
	for (unsigned i = 0; i < HAIKU_WL_BUFFER_COUNT; i++) {
		if (surface->wl_buffers[i].proxy != NULL) {
			hdisp->wl.marshal_flags((wl_proxy*)surface->wl_buffers[i].proxy,
				WL_BUFFER_DESTROY, NULL, 0, WL_MARSHAL_FLAG_DESTROY);
			surface->wl_buffers[i].proxy = NULL;
		}
		surface->wl_buffers[i].data = NULL;
		surface->wl_buffers[i].busy = false;
	}
	if (surface->wl_map != NULL)
		munmap(surface->wl_map, surface->wl_map_size);
	surface->wl_map = NULL;
	surface->wl_map_size = 0;
	surface->wl_stride = 0;
	surface->wl_width = 0;
	surface->wl_height = 0;
	surface->wl_next_buffer = 0;
}


static bool
create_wayland_buffers(struct haiku_egl_surface* surface,
	struct haiku_egl_display* hdisp,
	uint32_t width, uint32_t height)
{
	destroy_wayland_buffers(surface, hdisp);

	if (width == 0 || height == 0 || width > UINT32_MAX / 4)
		return false;
	uint32_t stride = width * 4;
	uint64_t one_size = (uint64_t)stride * height;
	uint64_t total_size = one_size * HAIKU_WL_BUFFER_COUNT;
	if (one_size == 0 || total_size > INT32_MAX || total_size > SIZE_MAX)
		return false;

	int fd = create_shm_file();
	if (fd < 0) {
		ERROR("could not create Wayland shm file: %s\n", strerror(errno));
		return false;
	}
	if (ftruncate(fd, (off_t)total_size) != 0) {
		ERROR("could not size Wayland shm file: %s\n", strerror(errno));
		close(fd);
		return false;
	}

	void* map = mmap(NULL, (size_t)total_size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		ERROR("could not map Wayland shm file: %s\n", strerror(errno));
		close(fd);
		return false;
	}

	uint32_t shm_version = hdisp->wl.get_version((wl_proxy*)hdisp->wayland_shm);
	wl_shm_pool* pool = (wl_shm_pool*)hdisp->wl.marshal_flags(
		(wl_proxy*)hdisp->wayland_shm, WL_SHM_CREATE_POOL,
		hdisp->wl.shm_pool_interface, shm_version, 0, NULL, fd,
		(int32_t)total_size);
	close(fd);
	if (pool == NULL) {
		munmap(map, (size_t)total_size);
		return false;
	}

	uint32_t format = surface->base.Config->AlphaSize > 0
		? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
	uint32_t pool_version = hdisp->wl.get_version((wl_proxy*)pool);
	bool ok = true;
	for (unsigned i = 0; i < HAIKU_WL_BUFFER_COUNT; i++) {
		haiku_wayland_buffer& buffer = surface->wl_buffers[i];
		buffer.proxy = (wl_buffer*)hdisp->wl.marshal_flags((wl_proxy*)pool,
			WL_SHM_POOL_CREATE_BUFFER, hdisp->wl.buffer_interface,
			pool_version, 0, NULL, (int32_t)(one_size * i), (int32_t)width,
			(int32_t)height, (int32_t)stride, format);
		buffer.data = (uint8_t*)map + one_size * i;
		buffer.busy = false;
		if (buffer.proxy == NULL
			|| hdisp->wl.add_listener((wl_proxy*)buffer.proxy,
				(void (**)(void))&kWaylandBufferListener, &buffer) < 0) {
			ok = false;
			break;
		}
	}

	hdisp->wl.marshal_flags((wl_proxy*)pool, WL_SHM_POOL_DESTROY, NULL, 0,
		WL_MARSHAL_FLAG_DESTROY);
	if (!ok) {
		surface->wl_map = map;
		surface->wl_map_size = (size_t)total_size;
		destroy_wayland_buffers(surface, hdisp);
		return false;
	}

	surface->wl_map = map;
	surface->wl_map_size = (size_t)total_size;
	surface->wl_stride = stride;
	surface->wl_width = width;
	surface->wl_height = height;
	return true;
}


static void
wayland_window_resized(wl_egl_window* window, void* data)
{
	struct haiku_egl_surface* surface = (struct haiku_egl_surface*)data;
	if (window->width <= 0 || window->height <= 0)
		return;
	surface->base.Width = window->width;
	surface->base.Height = window->height;
	if (surface->shim != NULL) {
		surface->shim->width = window->width;
		surface->shim->height = window->height;
	}
	if (surface->buffer != NULL)
		p_atomic_inc(&surface->buffer->stfbi->stamp);
}


static void
wayland_window_destroyed(void* data)
{
	/* The application violated EGL's preferred teardown order, but retaining
	 * the EGLSurface is harmless. SwapBuffers will now report BAD_SURFACE. */
	((struct haiku_egl_surface*)data)->wl_window = NULL;
}


extern "C" _EGLSurface*
haiku_create_window_surface(_EGLDisplay* disp, _EGLConfig* conf,
	void* native_window, const EGLint* attrib_list)
{
	struct haiku_egl_display* hdisp = haiku_egl_display(disp);
	DisplayLock guard(hdisp);
	BWindow* window = NULL;
	wl_egl_window* wl_window = NULL;
	if (native_window == NULL) {
		_eglError(EGL_BAD_NATIVE_WINDOW, "haiku_create_window_surface");
		return NULL;
	}

	intptr_t native_version;
	memcpy(&native_version, native_window, sizeof(native_version));
	if (native_version == WL_EGL_WINDOW_VERSION) {
		wl_window = (wl_egl_window*)native_window;
		if (!init_wayland_display(hdisp, disp->PlatformDisplay)
			|| wl_window->surface == NULL
			|| wl_window->width <= 0 || wl_window->height <= 0
			|| strcmp(hdisp->wl.get_class((wl_proxy*)wl_window->surface),
				"wl_surface") != 0
			|| wl_window->driver_private != NULL
			|| wl_window->resize_callback != NULL
			|| wl_window->destroy_window_callback != NULL) {
			_eglError(EGL_BAD_NATIVE_WINDOW,
				"haiku_create_window_surface: invalid or busy wl_egl_window");
			return NULL;
		}
	} else {
		window = (BWindow*)native_window;
	}

	struct haiku_egl_surface* surface = CALLOC_STRUCT(haiku_egl_surface);
	if (!surface) {
		_eglError(EGL_BAD_ALLOC, "haiku_create_window_surface");
		return NULL;
	}

	if (!_eglInitSurface(&surface->base, disp, EGL_WINDOW_BIT, conf,
			attrib_list, native_window)) {
		free(surface);
		return NULL;
	}

	if (wl_window != NULL) {
		surface->wl_native = true;
		surface->wl_window = wl_window;
		surface->base.Width = wl_window->width;
		surface->base.Height = wl_window->height;
	} else {
		if (!window->Lock()) {
			_eglError(EGL_BAD_NATIVE_WINDOW,
				"haiku_create_window_surface: could not lock the window");
			free(surface);
			return NULL;
		}

		BRect bounds = window->Bounds();
		surface->window = window;
		surface->view = new BView(bounds, "EGL", B_FOLLOW_ALL_SIDES,
			B_WILL_DRAW);
		window->AddChild(surface->view);
		window->Unlock();

		surface->base.Width = (EGLint)bounds.IntegerWidth() + 1;
		surface->base.Height = (EGLint)bounds.IntegerHeight() + 1;
	}

	surface->shim = CALLOC_STRUCT(hgl_context);
	if (!surface->shim) {
		_eglError(EGL_BAD_ALLOC, "haiku_create_window_surface");
		goto cleanup;
	}
	surface->shim->display = hdisp->display;
	surface->shim->stVisual = create_visual(haiku_egl_config(conf));
	if (!surface->shim->stVisual) {
		_eglError(EGL_BAD_ALLOC, "haiku_create_window_surface");
		goto cleanup;
	}
	surface->shim->width = surface->base.Width;
	surface->shim->height = surface->base.Height;

	surface->buffer = hgl_create_st_framebuffer(surface->shim, NULL);
	if (!surface->buffer) {
		_eglError(EGL_BAD_ALLOC, "haiku_create_window_surface");
		goto cleanup;
	}

	if (wl_window != NULL) {
		wl_window->driver_private = surface;
		wl_window->resize_callback = wayland_window_resized;
		wl_window->destroy_window_callback = wayland_window_destroyed;
		INFO("created %dx%d wl_egl_window surface\n", wl_window->width,
			wl_window->height);
	}

	return &surface->base;

cleanup:
	if (surface->view != NULL && window->Lock()) {
		surface->view->RemoveSelf();
		delete surface->view;
		window->Unlock();
	}
	if (surface->shim != NULL) {
		if (surface->shim->stVisual)
			free(surface->shim->stVisual);
		free(surface->shim);
	}
	free(surface);
	return NULL;
}


/* Read the finished colour buffer back and blit it into the surface's view.
 *
 * The Iris winsys has no scanout path of its own on Haiku, so presentation is
 * the same CPU readback the BGLView renderer performs: rendering happens on
 * the GPU, only the final copy crosses to the app_server. */
static void
present_window(struct haiku_egl_surface* surface, struct st_context_iface* stctxi)
{
	if (surface->window == NULL || surface->view == NULL)
		return;

	enum st_attachment_type statt =
		(surface->buffer->visual->buffer_mask & ST_ATTACHMENT_BACK_LEFT_MASK)
			? ST_ATTACHMENT_BACK_LEFT : ST_ATTACHMENT_FRONT_LEFT;

	struct pipe_resource* resource = surface->buffer->textures[statt];
	if (resource == NULL)
		return;

	struct pipe_context* pipe = ((struct st_context*)stctxi)->pipe;
	if (pipe == NULL)
		return;

	struct pipe_transfer* transfer = NULL;
	void* pixels = pipe_texture_map(pipe, resource, 0, 0, PIPE_MAP_READ,
		0, 0, resource->width0, resource->height0, &transfer);
	if (pixels == NULL) {
		ERROR("framebuffer readback failed\n");
		return;
	}

	BRect frame(0, 0, resource->width0 - 1, resource->height0 - 1);
	if (surface->bitmap == NULL || surface->bitmap->Bounds() != frame) {
		delete surface->bitmap;
		surface->bitmap = new BBitmap(frame, B_RGB32);
	}

	if (surface->bitmap != NULL && surface->bitmap->InitCheck() == B_OK) {
		util_format_translate(PIPE_FORMAT_B8G8R8X8_UNORM,
			surface->bitmap->Bits(), surface->bitmap->BytesPerRow(), 0, 0,
			resource->format, pixels, transfer->stride, 0, 0,
			resource->width0, resource->height0);

		if (surface->window->Lock()) {
			surface->view->DrawBitmap(surface->bitmap, BPoint(0, 0));
			surface->view->Flush();
			surface->window->Unlock();
		}
	}

	pipe_texture_unmap(pipe, transfer);
}


static bool
present_wayland(struct haiku_egl_surface* surface,
	struct haiku_egl_display* hdisp, struct st_context_iface* stctxi)
{
	if (surface->wl_window == NULL)
		return false;

	enum st_attachment_type statt =
		(surface->buffer->visual->buffer_mask & ST_ATTACHMENT_BACK_LEFT_MASK)
			? ST_ATTACHMENT_BACK_LEFT : ST_ATTACHMENT_FRONT_LEFT;
	struct pipe_resource* resource = surface->buffer->textures[statt];
	if (resource == NULL)
		return false;
	struct pipe_context* pipe = ((struct st_context*)stctxi)->pipe;
	if (pipe == NULL)
		return false;

	if (hdisp->wl.dispatch_queue_pending(hdisp->wayland_display,
			hdisp->wl_queue) < 0)
		return false;

	uint32_t width = resource->width0;
	uint32_t height = resource->height0;
	if (surface->wl_map == NULL || surface->wl_width != width
		|| surface->wl_height != height) {
		if (!create_wayland_buffers(surface, hdisp, width, height)) {
			ERROR("could not create %ux%u Wayland presentation buffers\n",
				width, height);
			return false;
		}
	}

	haiku_wayland_buffer* buffer = NULL;
	for (unsigned pass = 0; pass < 2 && buffer == NULL; pass++) {
		for (unsigned i = 0; i < HAIKU_WL_BUFFER_COUNT; i++) {
			unsigned index = (surface->wl_next_buffer + i)
				% HAIKU_WL_BUFFER_COUNT;
			if (!surface->wl_buffers[index].busy) {
				buffer = &surface->wl_buffers[index];
				surface->wl_next_buffer = (index + 1)
					% HAIKU_WL_BUFFER_COUNT;
				break;
			}
		}
		if (buffer == NULL && hdisp->wl.roundtrip_queue(hdisp->wayland_display,
				hdisp->wl_queue) < 0)
			return false;
	}
	if (buffer == NULL) {
		ERROR("Wayland compositor retained every presentation buffer\n");
		return false;
	}

	struct pipe_transfer* transfer = NULL;
	void* pixels = pipe_texture_map(pipe, resource, 0, 0, PIPE_MAP_READ,
		0, 0, width, height, &transfer);
	if (pixels == NULL) {
		ERROR("Wayland framebuffer readback failed\n");
		return false;
	}

	enum pipe_format target_format = surface->base.Config->AlphaSize > 0
		? PIPE_FORMAT_B8G8R8A8_UNORM : PIPE_FORMAT_B8G8R8X8_UNORM;
	util_format_translate(target_format, buffer->data, surface->wl_stride,
		0, 0, resource->format, pixels, transfer->stride, 0, 0,
		width, height);
	pipe_texture_unmap(pipe, transfer);

	buffer->busy = true;
	wl_egl_window* window = surface->wl_window;
	wl_proxy* wl_surface_proxy = (wl_proxy*)window->surface;
	uint32_t surface_version = hdisp->wl.get_version(wl_surface_proxy);
	hdisp->wl.marshal_flags(wl_surface_proxy, WL_SURFACE_ATTACH, NULL,
		surface_version, 0, buffer->proxy, window->dx, window->dy);
	if (surface_version >= 4) {
		hdisp->wl.marshal_flags(wl_surface_proxy, WL_SURFACE_DAMAGE_BUFFER,
			NULL, surface_version, 0, 0, 0, (int32_t)width,
			(int32_t)height);
	} else {
		hdisp->wl.marshal_flags(wl_surface_proxy, WL_SURFACE_DAMAGE, NULL,
			surface_version, 0, 0, 0, (int32_t)width, (int32_t)height);
	}
	hdisp->wl.marshal_flags(wl_surface_proxy, WL_SURFACE_COMMIT, NULL,
		surface_version, 0);

	/* Push the attach/damage/commit out to the compositor now.
	 *
	 * Requests are queued in the client's outgoing buffer until something
	 * flushes the display, and nothing on this path does: roundtrip_queue only
	 * runs when every buffer is already busy. Leaving the frame queued means it
	 * is presented whenever the toolkit's event loop next happens to flush,
	 * which under a heavy page is irregular, and the wl_buffer.release events
	 * that free our buffers come back just as late. All three buffers then look
	 * busy, this function fails, eglSwapBuffers fails with it, and the
	 * compositor falls back to software rendering. */
	hdisp->wl.display_flush(hdisp->wayland_display);

	window->attached_width = width;
	window->attached_height = height;
	window->dx = 0;
	window->dy = 0;
	return true;
}


extern "C" _EGLSurface*
haiku_create_pixmap_surface(_EGLDisplay* disp, _EGLConfig* conf,
	void* native_pixmap, const EGLint* attrib_list)
{
	_eglError(EGL_BAD_NATIVE_PIXMAP,
		"haiku_create_pixmap_surface: pixmap surfaces are not supported");
	return NULL;
}


extern "C" EGLBoolean
haiku_destroy_surface(_EGLDisplay* disp, _EGLSurface* surf)
{
	struct haiku_egl_surface* surface = haiku_egl_surface(surf);
	struct haiku_egl_display* hdisp = haiku_egl_display(disp);
	DisplayLock guard(hdisp);

	if (_eglPutSurface(surf)) {
		if (surface->wl_window != NULL
			&& surface->wl_window->driver_private == surface) {
			surface->wl_window->driver_private = NULL;
			surface->wl_window->resize_callback = NULL;
			surface->wl_window->destroy_window_callback = NULL;
		}
		if (surface->wl_native)
			destroy_wayland_buffers(surface, hdisp);
		if (surface->buffer)
			hgl_destroy_st_framebuffer(surface->buffer);
		if (surface->view != NULL && surface->window != NULL
			&& surface->window->Lock()) {
			surface->view->RemoveSelf();
			delete surface->view;
			surface->window->Unlock();
		}
		delete surface->bitmap;
		if (surface->shim) {
			if (surface->shim->stVisual)
				free(surface->shim->stVisual);
			free(surface->shim);
		}
		free(surface);
	}
	return EGL_TRUE;
}


extern "C" EGLBoolean
haiku_make_current(_EGLDisplay* disp, _EGLSurface* dsurf, _EGLSurface* rsurf,
	_EGLContext* ctx)
{
	struct haiku_egl_display* hdisp = haiku_egl_display(disp);
	DisplayLock guard(hdisp);
	struct haiku_egl_context* context = haiku_egl_context(ctx);
	_EGLContext* oldCtx;
	_EGLSurface* oldDraw;
	_EGLSurface* oldRead;

	if (!_eglBindContext(ctx, dsurf, rsurf, &oldCtx, &oldDraw, &oldRead))
		return EGL_FALSE;

	/* dsurf and rsurf are NULL for a surfaceless context, which the state
	 * tracker handles by binding an incomplete framebuffer. That is the path
	 * WebGL takes: it renders into FBOs it creates itself. */
	struct st_framebuffer_iface* draw = NULL;
	struct st_framebuffer_iface* read = NULL;
	if (dsurf != NULL)
		draw = haiku_egl_surface(dsurf)->buffer->stfbi;
	if (rsurf != NULL)
		read = haiku_egl_surface(rsurf)->buffer->stfbi;

	/* hgl_st_framebuffer_validate sizes the attachments from the context, not
	 * from the framebuffer, so the drawable size has to be published here
	 * before the state tracker validates. */
	if (context != NULL && dsurf != NULL) {
		context->hgl.width = (unsigned)dsurf->Width;
		context->hgl.height = (unsigned)dsurf->Height;
	}

	bool ok;
	if (context != NULL) {
		ok = hdisp->display->api->make_current(hdisp->display->api,
			context->st, draw, read);
	} else {
		ok = hdisp->display->api->make_current(hdisp->display->api,
			NULL, NULL, NULL);
	}

	if (!ok) {
		/* Put the previous binding back so a failed eglMakeCurrent does not
		 * leave the thread without a context. */
		_eglBindContext(oldCtx, oldDraw, oldRead, &ctx, &dsurf, &rsurf);
		return _eglError(EGL_BAD_MATCH, "haiku_make_current");
	}

	if (oldCtx != NULL)
		haiku_destroy_context(disp, oldCtx);
	if (oldDraw != NULL)
		haiku_destroy_surface(disp, oldDraw);
	if (oldRead != NULL && oldRead != oldDraw)
		haiku_destroy_surface(disp, oldRead);

	return EGL_TRUE;
}


extern "C" EGLBoolean
haiku_swap_buffers(_EGLDisplay* disp, _EGLSurface* surf)
{
	struct haiku_egl_display* hdisp = haiku_egl_display(disp);
	struct haiku_egl_surface* surface = haiku_egl_surface(surf);
	DisplayLock guard(hdisp);

	_EGLContext* current = _eglGetCurrentContext();
	struct st_context_iface* st = current != NULL
		? haiku_egl_context(current)->st : NULL;
	if (st == NULL)
		return _eglError(EGL_BAD_CONTEXT, "haiku_swap_buffers");
	if (surface->wl_native) {
		if (surface->wl_window == NULL)
			return _eglError(EGL_BAD_SURFACE,
				"haiku_swap_buffers: wl_egl_window was destroyed");
		unsigned width = surface->wl_window->width;
		unsigned height = surface->wl_window->height;
		if (width == 0 || height == 0)
			return _eglError(EGL_BAD_SURFACE,
				"haiku_swap_buffers: invalid wl_egl_window size");
		if (width != (unsigned)surface->base.Width
			|| height != (unsigned)surface->base.Height) {
			surface->base.Width = width;
			surface->base.Height = height;
			p_atomic_inc(&surface->buffer->stfbi->stamp);
		}
		haiku_egl_context(current)->hgl.width = width;
		haiku_egl_context(current)->hgl.height = height;
	}

	/* Finish the frame before it is read back or handed on. */
	st->flush(st, ST_FLUSH_FRONT, NULL, NULL, NULL);

	/* EGL says eglSwapBuffers on a pbuffer is a no-op; the flush above is all
	 * a following glReadPixels or resource share needs. */
	if (surface->window == NULL && !surface->wl_native)
		return EGL_TRUE;

	if (surface->wl_native) {
		if (!present_wayland(surface, haiku_egl_display(disp), st))
			return _eglError(EGL_BAD_SURFACE,
				"haiku_swap_buffers: Wayland presentation failed");
	} else {
		present_window(surface, st);
	}

	/* Pick up a resize for the next frame. hgl_st_framebuffer_validate
	 * compares the buffer against the hgl_context's size and reallocates the
	 * attachments when they differ, so recording it here is enough. */
	if (surface->window != NULL && surface->window->Lock()) {
		BRect bounds = surface->window->Bounds();
		surface->window->Unlock();

		unsigned width = (unsigned)bounds.IntegerWidth() + 1;
		unsigned height = (unsigned)bounds.IntegerHeight() + 1;
		if (width != (unsigned)surface->base.Width
			|| height != (unsigned)surface->base.Height) {
			surface->base.Width = (EGLint)width;
			surface->base.Height = (EGLint)height;
			/* The size validate() reads lives on the context, and the app may
			 * keep swapping without ever calling eglMakeCurrent again. */
			haiku_egl_context(current)->hgl.width = width;
			haiku_egl_context(current)->hgl.height = height;
			p_atomic_inc(&surface->buffer->stfbi->stamp);
		}
	}

	return EGL_TRUE;
}


extern "C" EGLBoolean
haiku_wait_client(_EGLDisplay* disp, _EGLContext* ctx)
{
	struct haiku_egl_display* hdisp = haiku_egl_display(disp);
	struct haiku_egl_context* context = haiku_egl_context(ctx);
	DisplayLock guard(hdisp);
	if (context != NULL && context->st != NULL)
		context->st->flush(context->st, ST_FLUSH_FRONT, NULL, NULL, NULL);
	return EGL_TRUE;
}


extern "C" EGLBoolean
haiku_wait_native(EGLint engine)
{
	if (engine != EGL_CORE_NATIVE_ENGINE)
		return _eglError(EGL_BAD_PARAMETER, "haiku_wait_native");
	return EGL_TRUE;
}


extern "C" const char*
haiku_query_driver_name(_EGLDisplay* disp)
{
	/* The Gallium driver behind the screen. Reported by EGL_MESA_query_driver
	 * and shown by consumers such as Firefox's about:support. */
	return "iris";
}


extern "C" char*
haiku_query_driver_config(_EGLDisplay* disp)
{
	/* driconf XML for the driver. Nothing is configurable through EGL here, so
	 * hand back an empty but well-formed document; the caller frees it. */
	static const char kEmptyDriConf[] =
		"<?xml version=\"1.0\" standalone=\"yes\"?>\n"
		"<!DOCTYPE driinfo [\n"
		"   <!ELEMENT driinfo (section*)>\n"
		"]>\n"
		"<driinfo>\n</driinfo>\n";
	return strdup(kEmptyDriConf);
}


extern "C" _EGLProc
haiku_get_proc_address(const char* procname)
{
	return (_EGLProc)_glapi_get_proc_address(procname);
}


extern "C" const _EGLDriver _eglDriver = {
	.Initialize = init_haiku,
	.Terminate = haiku_terminate,
	.CreateContext = haiku_create_context,
	.DestroyContext = haiku_destroy_context,
	.MakeCurrent = haiku_make_current,
	.CreateWindowSurface = haiku_create_window_surface,
	.CreatePixmapSurface = haiku_create_pixmap_surface,
	.CreatePbufferSurface = haiku_create_pbuffer_surface,
	.DestroySurface = haiku_destroy_surface,
	.SwapBuffers = haiku_swap_buffers,
	.WaitClient = haiku_wait_client,
	.WaitNative = haiku_wait_native,
	.GetProcAddress = haiku_get_proc_address,
	.QueryDriverName = haiku_query_driver_name,
	.QueryDriverConfig = haiku_query_driver_config,
};
