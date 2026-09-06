/*
 * Copyright 2012, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Artur Wyszynski, harakash@gmail.com
 *		Alexander von Gluck IV, kallisti5@unixzen.com
 */


#include "GalliumContext.h"

#include <stdlib.h>

#include <stdio.h>
#include <algorithm>
#include <mutex>
static std::mutex sDisplayLock;

#include "GLView.h"

#include "bitmap_wrapper.h"

#include "glapi/glapi.h"
#include "pipe/p_format.h"
//#include "state_tracker/st_cb_fbo.h"
//#include "state_tracker/st_cb_flush.h"
#include "state_tracker/st_context.h"
#include "state_tracker/st_gl_api.h"
#include "frontend/sw_winsys.h"
#include "sw/hgl/hgl_sw_winsys.h"
#include "util/u_atomic.h"
#include "util/u_memory.h"
#include "util/u_framebuffer.h"

#include "IntelScreen.h"
#include "target-helpers/inline_debug_helper.h"


#ifdef DEBUG
#	define TRACE(x...) printf("GalliumContext: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
#	define CALLED()
#endif
#define ERROR(x...) printf("GalliumContext: " x)

int32 GalliumContext::fDisplayRefCount = 0;
hgl_display* GalliumContext::fDisplay = NULL;

GalliumContext::GalliumContext(ulong options)
	:
	fOptions(options),
	fCurrentContext(0)
{
	CALLED();

	// The lock has to exist before anything can take it, and something can:
	// the window's thread draws the view as soon as it is attached, which is
	// while this constructor is still running. Creating the display first left
	// a window in which Draw took a mutex that had never been initialised.
	(void) mtx_init(&fMutex, mtx_plain);

	// Make all contexts a known value
	for (context_id i = 0; i < CONTEXT_MAX; i++)
		fContext[i] = NULL;

	CreateDisplay();
}


GalliumContext::~GalliumContext()
{
	CALLED();

	// Destroy our contexts
	Lock();
	for (context_id i = 0; i < CONTEXT_MAX; i++)
		DestroyContext(i);
	Unlock();

	DestroyDisplay();

	mtx_destroy(&fMutex);
}


status_t
GalliumContext::CreateDisplay()
{
	std::lock_guard<std::mutex> guard(sDisplayLock);
	CALLED();

	if (atomic_add(&fDisplayRefCount, 1) > 0)
		return B_OK;

	struct pipe_screen* screen = intel_haiku_screen_create();
	if (screen == NULL) {
		atomic_add(&fDisplayRefCount, -1);
		return B_ERROR;
	}

	debug_screen_wrap(screen);

	const char* driverName = screen->get_name(screen);
	ERROR("%s: Using %s driver.\n", __func__, driverName);

	fDisplay = hgl_create_display(screen);

	if (fDisplay == NULL) {
		ERROR("%s: Couldn't create display!\n", __FUNCTION__);
		screen->destroy(screen); // will also destroy winsys
		atomic_add(&fDisplayRefCount, -1);
		return B_ERROR;
	}

	return B_OK;
}


void
GalliumContext::DestroyDisplay()
{
	std::lock_guard<std::mutex> guard(sDisplayLock);
	if (atomic_add(&fDisplayRefCount, -1) > 1)
		return;

	if (fDisplay != NULL && getenv("INTEL_GFX_KEEP_SCREEN") == NULL) {
		struct pipe_screen* screen = fDisplay->manager->screen;
		hgl_destroy_display(fDisplay); fDisplay = NULL;
		screen->destroy(screen); // destroy will deallocate object
	}
}


context_id
GalliumContext::CreateContext(HGLWinsysContext *wsContext)
{
	CALLED();

	struct hgl_context* context = CALLOC_STRUCT(hgl_context);

	if (!context) {
		ERROR("%s: Couldn't create pipe context!\n", __FUNCTION__);
		return -1;
	}

	// Set up the initial things our context needs
	if (fDisplay == NULL) {
		FREE(context);
		return -1;
	}
	context->display = fDisplay;

	// Create state tracker visual
	context->stVisual = hgl_create_st_visual(fOptions);

	// Create state tracker framebuffers
	context->buffer = hgl_create_st_framebuffer(context, wsContext);

	if (!context->buffer) {
		ERROR("%s: Problem allocating framebuffer!\n", __func__);
		hgl_destroy_st_visual(context->stVisual);
		FREE(context);
		return -1;
	}

	// Build state tracker attributes
	struct st_context_attribs attribs;
	memset(&attribs, 0, sizeof(attribs));
	attribs.options.force_glsl_extensions_warn = false;
	attribs.profile = ST_PROFILE_DEFAULT;
	attribs.visual = *context->stVisual;
	attribs.major = 1;
	attribs.minor = 0;
	//attribs.flags |= ST_CONTEXT_FLAG_DEBUG;

	struct st_context_iface* shared = NULL;

	if (fOptions & BGL_SHARE_CONTEXT) {
		shared = fDisplay->api->get_current(fDisplay->api);
		TRACE("shared context: %p\n", shared);
	}

	// Create context using state tracker api call
	enum st_context_error result;
	context->st = fDisplay->api->create_context(fDisplay->api, fDisplay->manager,
		&attribs, &result, shared);

	if (!context->st) {
		ERROR("%s: Couldn't create mesa state tracker context!\n",
			__func__);
		switch (result) {
			case ST_CONTEXT_SUCCESS:
				ERROR("%s: State tracker error: SUCCESS?\n", __func__);
				break;
			case ST_CONTEXT_ERROR_NO_MEMORY:
				ERROR("%s: State tracker error: NO_MEMORY\n", __func__);
				break;
			case ST_CONTEXT_ERROR_BAD_API:
				ERROR("%s: State tracker error: BAD_API\n", __func__);
				break;
			case ST_CONTEXT_ERROR_BAD_VERSION:
				ERROR("%s: State tracker error: BAD_VERSION\n", __func__);
				break;
			case ST_CONTEXT_ERROR_BAD_FLAG:
				ERROR("%s: State tracker error: BAD_FLAG\n", __func__);
				break;
			case ST_CONTEXT_ERROR_UNKNOWN_ATTRIBUTE:
				ERROR("%s: State tracker error: BAD_ATTRIBUTE\n", __func__);
				break;
			case ST_CONTEXT_ERROR_UNKNOWN_FLAG:
				ERROR("%s: State tracker error: UNKNOWN_FLAG\n", __func__);
				break;
		}

		hgl_destroy_st_visual(context->stVisual);
		FREE(context);
		return -1;
	}

	assert(!context->st->st_manager_private);
	context->st->st_manager_private = (void*)context;

	struct st_context *stContext = (struct st_context*)context->st;

	// Init Gallium3D Post Processing
	// TODO: no pp filters are enabled yet through postProcessEnable
	context->postProcess = pp_init(stContext->pipe, context->postProcessEnable,
		stContext->cso_context, &stContext->iface);

	context_id contextNext = -1;
	Lock();
	for (context_id i = 0; i < CONTEXT_MAX; i++) {
		if (fContext[i] == NULL) {
			fContext[i] = context;
			contextNext = i;
			break;
		}
	}
	Unlock();

	if (contextNext < 0) {
		ERROR("%s: The next context is invalid... something went wrong!\n",
			__func__);
		if (context->postProcess)
			pp_free(context->postProcess);
		context->st->destroy(context->st);
		hgl_destroy_st_framebuffer(context->buffer);
		hgl_destroy_st_visual(context->stVisual);
		FREE(context);
		return -1;
	}

	TRACE("%s: context #%" B_PRIu64 " is the next available context\n",
		__func__, contextNext);

	return contextNext;
}


void
GalliumContext::DestroyContext(context_id contextID)
{
	// fMutex should be locked *before* calling DestoryContext

	// See if context is used
	if (!fContext[contextID])
		return;

	if (fContext[contextID]->st) {
		fContext[contextID]->st->flush(fContext[contextID]->st, 0, NULL, NULL, NULL);
		fContext[contextID]->st->destroy(fContext[contextID]->st);
	}

	if (fContext[contextID]->postProcess)
		pp_free(fContext[contextID]->postProcess);

	// Delete state tracker framebuffer objects
	if (fContext[contextID]->buffer)
		hgl_destroy_st_framebuffer(fContext[contextID]->buffer);

	if (fContext[contextID]->stVisual)
		hgl_destroy_st_visual(fContext[contextID]->stVisual);

	FREE(fContext[contextID]);
	fContext[contextID] = NULL;
}


status_t
GalliumContext::SetCurrentContext(bool set, context_id contextID)
{
	CALLED();

	if (contextID < 0 || contextID >= CONTEXT_MAX) {
		ERROR("%s: Invalid context ID range!\n", __func__);
		return B_ERROR;
	}

	Lock();
	context_id oldContextID = fCurrentContext;
	struct hgl_context* context = fContext[contextID];

	if (!context) {
		ERROR("%s: Invalid context provided (#%" B_PRIu64 ")!\n",
			__func__, contextID);
		Unlock();
		return B_ERROR;
	}

	if (!set) {
		fDisplay->api->make_current(fDisplay->api, NULL, NULL, NULL);
		Unlock();
		return B_OK;
	}

	// Everything seems valid, lets set the new context.
	fCurrentContext = contextID;

	if (oldContextID > 0 && oldContextID != contextID) {
		fContext[oldContextID]->st->flush(fContext[oldContextID]->st,
			ST_FLUSH_FRONT, NULL, NULL, NULL);
	}

	// We need to lock and unlock framebuffers before accessing them
	fDisplay->api->make_current(fDisplay->api, context->st, context->buffer->stfbi,
		context->buffer->stfbi);
	Unlock();

	return B_OK;
}


status_t
GalliumContext::SwapBuffers(context_id contextID)
{
	CALLED();

	Lock();
	struct hgl_context* context = fContext[contextID];

	if (!context) {
		ERROR("%s: context not found\n", __func__);
		Unlock();
		return B_ERROR;
	}

	// will flush front buffer if no double buffering is used
	context->st->flush(context->st, ST_FLUSH_FRONT, NULL, NULL, NULL);

	struct hgl_buffer* buffer = context->buffer;

	// flush back buffer and swap buffers if double buffering is used
	if (buffer->textures[ST_ATTACHMENT_BACK_LEFT] != NULL) {
		buffer->screen->flush_frontbuffer(buffer->screen, ((struct st_context*)context->st)->pipe, buffer->textures[ST_ATTACHMENT_BACK_LEFT],
			0, 0, buffer->winsysContext, NULL);
		std::swap(buffer->textures[ST_ATTACHMENT_FRONT_LEFT], buffer->textures[ST_ATTACHMENT_BACK_LEFT]);
		p_atomic_inc(&buffer->stfbi->stamp);
	}

	Unlock();
	return B_OK;
}


void
GalliumContext::Draw(context_id contextID, BRect updateRect)
{
	// Draw arrives on the window's thread whenever the view is exposed, while
	// the application's own thread is drawing and swapping. Both end up
	// flushing the same pipe context, which is not shared state a Gallium
	// driver tolerates: two threads inside one batch flush corrupt its buffer
	// list. Take the same lock SwapBuffers takes.
	if (contextID < 0 || contextID >= CONTEXT_MAX)
		return;

	Lock();
	struct hgl_context *context = fContext[contextID];

	if (!context) {
		// Not an error: the view can be asked to draw before its context has
		// been created and after it has been destroyed.
		Unlock();
		return;
	}

	struct hgl_buffer* buffer = context->buffer;

	if (buffer->textures[ST_ATTACHMENT_FRONT_LEFT] == NULL) {
		Unlock();
		return;
	}

	buffer->screen->flush_frontbuffer(buffer->screen, ((struct st_context*)context->st)->pipe, buffer->textures[ST_ATTACHMENT_FRONT_LEFT],
		0, 0, buffer->winsysContext, NULL);
	Unlock();
}


bool
GalliumContext::Validate(uint32 width, uint32 height)
{
	CALLED();

	if (!fContext[fCurrentContext])
		return false;

	if (fContext[fCurrentContext]->width != width + 1
		|| fContext[fCurrentContext]->height != height + 1) {
		Invalidate(width, height);
		return false;
	}
	return true;
}


void
GalliumContext::Invalidate(uint32 width, uint32 height)
{
	CALLED();

	assert(fContext[fCurrentContext]);

	// Update st_context dimensions 
	fContext[fCurrentContext]->width = width + 1;
	fContext[fCurrentContext]->height = height + 1;

	// Is this the best way to invalidate?
	p_atomic_inc(&fContext[fCurrentContext]->buffer->stfbi->stamp);
}


// Bring-up aid: INTEL_GFX_LOCK_TRACE=1 records who takes and drops the
// context lock, which is what a corrupt mutex needs in order to be traced back
// to the call that unbalanced it.
static bool
TraceLocks()
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = getenv("INTEL_GFX_LOCK_TRACE") != NULL ? 1 : 0;
	return enabled == 1;
}


void
GalliumContext::Lock()
{
	CALLED();
	if (TraceLocks()) {
		fprintf(stderr, "lock   want %" B_PRId32 " depth %" B_PRId32 "\n",
			find_thread(NULL), atomic_get(&fLockDepth));
	}
	mtx_lock(&fMutex);
	if (TraceLocks()) {
		atomic_add(&fLockDepth, 1);
		fprintf(stderr, "lock   have %" B_PRId32 " depth %" B_PRId32 "\n",
			find_thread(NULL), atomic_get(&fLockDepth));
	}
}


void
GalliumContext::Unlock()
{
	CALLED();
	if (TraceLocks()) {
		atomic_add(&fLockDepth, -1);
		fprintf(stderr, "unlock      %" B_PRId32 " depth %" B_PRId32 "\n",
			find_thread(NULL), atomic_get(&fLockDepth));
	}
	mtx_unlock(&fMutex);
}
/* vim: set tabstop=4: */
