/* SPDX-License-Identifier: MIT */
// A rotating cube, for finding out what draws it. The interesting part is
// not the cube: it is the renderer the GL kit gives us and how fast it goes,
// which is how the difference between a software rasteriser and a driver
// that uses this hardware will show itself.

#include <math.h>
#include <atomic>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <GLView.h>
#include <CheckBox.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <OptionPopUp.h>
#include <StringView.h>
#include <Window.h>

#include <GL/gl.h>
#include <GL/glu.h>

#include "CubeMessages.h"
#include "VulkanCubeView.h"


static bool sRequireHardware = false;
static uint32 sFrameLimit = 0;
static std::atomic<int> sExitCode(0);
static renderer_mode sRendererMode = RENDERER_OPENGL;

// Off by default, which is what this demo has always done: SwapBuffers()
// without an argument does not wait for the retrace. The checkbox exists so
// that a frame rate below the refresh rate can be shown not to be the display
// waiting, which is the first thing anyone suspects.
static std::atomic<bool> sVerticalSync(false);
static const bigtime_t kReportInterval = 1000000;


class CubeView : public BGLView {
public:
								CubeView();

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				FrameResized(float width, float height);

			void				Stop();

			BString				Renderer() const { return fRenderer; }

private:
	static	int32				_ThreadEntry(void* data);
			void				_Run();
			void				_Draw(float angle);
			void				_ReadStrings();

			void				_Start();

			thread_id			fThread;
			std::atomic<bool>	fRunning;
			// The drawing thread may not ask the view about itself: that
			// needs the window locked, and this thread is not the one that
			// holds it. The size it draws to is recorded here instead, by
			// the thread that is allowed to know it.
			std::atomic<int>	fWidth;
			std::atomic<int>	fHeight;
			BMessenger			fWindow;
			BString				fRenderer;
			BString				fVendor;
			BString				fVersion;
			GLuint				fCubeList;
};


CubeView::CubeView()
	:
	BGLView(BRect(0, 0, 1, 1), "cube", B_FOLLOW_ALL_SIDES, 0,
		BGL_RGB | BGL_DOUBLE | BGL_DEPTH),
	fThread(-1),
	fRunning(false),
	fWidth(1),
	fHeight(1),
	fCubeList(0)
{
	SetExplicitMinSize(BSize(320, 240));
}


void
CubeView::_ReadStrings()
{
	const char* renderer = (const char*)glGetString(GL_RENDERER);
	const char* vendor = (const char*)glGetString(GL_VENDOR);
	const char* version = (const char*)glGetString(GL_VERSION);
	fRenderer = renderer != NULL ? renderer : "unknown";
	fVendor = vendor != NULL ? vendor : "unknown";
	fVersion = version != NULL ? version : "unknown";

	// Printed as well as shown, so that this says something useful when it is
	// started from a terminal on another machine.
	printf("GL renderer: %s\n", fRenderer.String());
	printf("GL vendor:   %s\n", fVendor.String());
	printf("GL version:  %s\n", fVersion.String());
	fflush(stdout);
}


void
CubeView::_Draw(float angle)
{
	float width = fWidth.load();
	float height = fHeight.load();
	glViewport(0, 0, (GLint)width, (GLint)height);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(45.0, width / height, 1.0, 20.0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -4.5f);
	glRotatef(angle, 1.0f, 0.0f, 0.0f);
	glRotatef(angle * 0.7f, 0.0f, 1.0f, 0.0f);

	glEnable(GL_DEPTH_TEST);
	glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	static const GLfloat kFaces[6][12] = {
		{ -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1 },
		{ -1,-1,-1, -1, 1,-1,  1, 1,-1,  1,-1,-1 },
		{ -1, 1,-1, -1, 1, 1,  1, 1, 1,  1, 1,-1 },
		{ -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1 },
		{  1,-1,-1,  1, 1,-1,  1, 1, 1,  1,-1, 1 },
		{ -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1 }
	};
	static const GLfloat kColours[6][3] = {
		{ 0.85f, 0.25f, 0.18f }, { 0.20f, 0.60f, 0.85f },
		{ 0.95f, 0.75f, 0.20f }, { 0.30f, 0.75f, 0.40f },
		{ 0.65f, 0.35f, 0.80f }, { 0.90f, 0.90f, 0.90f }
	};

	if (fCubeList != 0) {
		glCallList(fCubeList);
		return;
	}
	fCubeList = glGenLists(1);
	if (fCubeList != 0)
		glNewList(fCubeList, GL_COMPILE_AND_EXECUTE);
	glBegin(GL_QUADS);
	for (int face = 0; face < 6; face++) {
		glColor3fv(kColours[face]);
		for (int vertex = 0; vertex < 4; vertex++)
			glVertex3fv(&kFaces[face][vertex * 3]);
	}
	glEnd();
	if (fCubeList != 0)
		glEndList();
}


int32
CubeView::_ThreadEntry(void* data)
{
	((CubeView*)data)->_Run();
	return 0;
}


void
CubeView::_Run()
{
	LockGL();
	_ReadStrings();
	UnlockGL();

	bool native = fRenderer.FindFirst("Iris / Haiku IntelGfx") >= 0;
	if (sRequireHardware && !native) {
		fprintf(stderr, "IntelGfx Iris required; selected renderer is %s\n",
			fRenderer.String());
		sExitCode = 1;
		fRunning = false;
		be_app->PostMessage(B_QUIT_REQUESTED);
		return;
	}

	uint32 frames = 0, totalFrames = 0;
	bigtime_t startedAt = system_time(), reportedAt = startedAt;
	bigtime_t completedTime = 0;
	bool pixelsChecked = false;
	while (fRunning.load()) {
		bigtime_t frameStart = system_time();
		float angle = fmod((frameStart - startedAt) * 0.00006, 360.0);
		LockGL();
		_Draw(angle);
		// Include completion, rather than measuring only CPU command dispatch.
		glFinish();
		if (!pixelsChecked) {
			GLubyte center[4] = {}, corner[4] = {};
			glReadPixels(fWidth.load() / 2, fHeight.load() / 2, 1, 1,
				GL_RGBA, GL_UNSIGNED_BYTE, center);
			glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
			bool visible = memcmp(center, corner, 3) != 0;
			printf("Readback: center=%u,%u,%u corner=%u,%u,%u (%s)\n",
				center[0], center[1], center[2], corner[0], corner[1], corner[2],
				visible ? "geometry visible" : "no geometry detected");
			if (!visible)
				sExitCode = 1;
			pixelsChecked = true;
		}
		GLenum error = glGetError();
		if (error != GL_NO_ERROR) {
			fprintf(stderr, "OpenGL error: 0x%x\n", error);
			sExitCode = 1;
		}
		SwapBuffers(sVerticalSync.load());
		UnlockGL();
		bigtime_t now = system_time();
		completedTime += now - frameStart;
		frames++;
		totalFrames++;
		if (now - reportedAt >= kReportInterval || totalFrames == sFrameLimit) {
			float fps = frames * 1000000.0 / (now - reportedAt);
			bigtime_t average = completedTime / frames;
			BMessage message(kStatistics);
			message.AddFloat("fps", fps);
			message.AddInt64("frame", average);
			message.AddString("renderer", fRenderer);
			message.AddString("vendor", fVendor);
			message.AddString("version", fVersion);
			message.AddString("mode", "OpenGL");
			fWindow.SendMessage(&message);
			printf("%.1f completed frames/s, %.2f ms/frame, %u frames\n",
				fps, average / 1000.0, totalFrames);
			fflush(stdout);
			frames = 0;
			completedTime = 0;
			reportedAt = now;
		}
		if (sExitCode != 0 || (sFrameLimit != 0 && totalFrames >= sFrameLimit)) {
			fRunning = false;
			be_app->PostMessage(B_QUIT_REQUESTED);
		}
	}
	LockGL();
	if (fCubeList != 0)
		glDeleteLists(fCubeList, 1);
	fCubeList = 0;
	UnlockGL();
}


void
CubeView::AttachedToWindow()
{
	BGLView::AttachedToWindow();

	BRect bounds = Bounds();
	fWidth = bounds.Width() + 1;
	fHeight = bounds.Height() + 1;
	fWindow = BMessenger(NULL, Window());

	_Start();
}


void
CubeView::DetachedFromWindow()
{
	Stop();
	BGLView::DetachedFromWindow();
}


void
CubeView::FrameResized(float width, float height)
{
	BGLView::FrameResized(width, height);
	fWidth = (int)fmax(1, width + 1);
	fHeight = (int)fmax(1, height + 1);
}


void
CubeView::_Start()
{
	if (fThread >= 0)
		return;
	fRunning = true;
	fThread = spawn_thread(_ThreadEntry, "cube", B_NORMAL_PRIORITY, this);
	if (fThread >= 0) {
		status_t status = resume_thread(fThread);
		if (status == B_OK)
			return;
		kill_thread(fThread);
		fThread = -1;
	}
	fRunning = false;
	sExitCode = 1;
	be_app->PostMessage(B_QUIT_REQUESTED);
}


void
CubeView::Stop()
{
	fRunning = false;
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}
}


class CubeWindow : public BWindow {
public:
								CubeWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			void				_SetRenderer(renderer_mode mode);
			void				_StopRenderer();

			BView*				fView;
			BGroupView*		fRenderGroup;
			BStringView*		fRendererView;
			BStringView*		fRateView;
			BCheckBox*			fVerticalSync;
			BOptionPopUp*		fRendererSelector;
};


CubeWindow::CubeWindow()
	:
	BWindow(BRect(80, 80, 720, 560), "Intel graphics cube", B_TITLED_WINDOW,
		B_QUIT_ON_WINDOW_CLOSE | B_AUTO_UPDATE_SIZE_LIMITS)
{
	fView = NULL;
	fRenderGroup = new BGroupView("renderer view", B_VERTICAL, 0);
	fRendererView = new BStringView("renderer", "Starting renderer…");
	fRateView = new BStringView("rate", "Measuring…");
	fRendererSelector = new BOptionPopUp("renderer selector", "Renderer:",
		new BMessage(kRendererChanged));
	fRendererSelector->AddOption("OpenGL", RENDERER_OPENGL);
	fRendererSelector->AddOption("Vulkan", RENDERER_VULKAN);
	fRendererSelector->SelectOptionFor(sRendererMode);
	fVerticalSync = new BCheckBox("vsync", "Wait for vertical retrace",
		new BMessage(kVerticalSync));
	fVerticalSync->SetValue(sVerticalSync.load() ? B_CONTROL_ON : B_CONTROL_OFF);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fRenderGroup)
		.AddGroup(B_VERTICAL, 2)
			.SetInsets(8, 6, 8, 6)
			.Add(fRendererView)
			.Add(fRateView)
			.Add(fRendererSelector)
			.Add(fVerticalSync)
			.End();
	_SetRenderer(sRendererMode);
}


void
CubeWindow::_StopRenderer()
{
	if (CubeView* view = dynamic_cast<CubeView*>(fView))
		view->Stop();
	else if (VulkanCubeView* view = dynamic_cast<VulkanCubeView*>(fView))
		view->Stop();
}


void
CubeWindow::_SetRenderer(renderer_mode mode)
{
	if (fView != NULL) {
		_StopRenderer();
		fRenderGroup->GroupLayout()->RemoveView(fView);
		delete fView;
	}
	sRendererMode = mode;
	fRendererView->SetText(mode == RENDERER_VULKAN
		? "Starting Vulkan ANV…" : "Asking the GL kit…");
	fRateView->SetText("Measuring…");
	fVerticalSync->SetEnabled(mode == RENDERER_OPENGL);
	if (mode == RENDERER_VULKAN) {
		fView = new VulkanCubeView(sRequireHardware, sFrameLimit, &sExitCode);
	} else
		fView = new CubeView();
	fRenderGroup->GroupLayout()->AddView(fView);
	fRenderGroup->GroupLayout()->Relayout(true);
}


void
CubeWindow::MessageReceived(BMessage* message)
{
	if (message->what == kVerticalSync) {
		sVerticalSync = fVerticalSync->Value() == B_CONTROL_ON;
		return;
	}
	if (message->what == kRendererChanged) {
		_SetRenderer((renderer_mode)fRendererSelector->Value());
		return;
	}
	if (message->what != kStatistics) {
		BWindow::MessageReceived(message);
		return;
	}

	BString renderer, vendor, version, mode, error;
	float fps = 0;
	int64 frame = 0;
	message->FindString("renderer", &renderer);
	message->FindString("vendor", &vendor);
	message->FindString("version", &version);
	message->FindString("mode", &mode);
	if (message->FindString("error", &error) == B_OK) {
		fRendererView->SetText(error);
		fRateView->SetText("Vulkan renderer stopped");
		return;
	}
	message->FindFloat("fps", &fps);
	message->FindInt64("frame", &frame);

	// A renderer that names a software rasteriser is worth saying out loud:
	// it is the difference this driver is meant to remove.
	bool software = renderer.IFindFirst("llvmpipe") >= 0
		|| renderer.IFindFirst("softpipe") >= 0
		|| renderer.IFindFirst("swrast") >= 0
		|| renderer.IFindFirst("software") >= 0;

	BString text;
	bool vulkan = mode == "Vulkan";
	bool native = vulkan || renderer.FindFirst("Iris / Haiku IntelGfx") >= 0;
	text.SetToFormat("%s — %s (%s)", software ? "Software rasteriser"
		: native ? "IntelGfx hardware rendering" : "Unverified renderer",
		renderer.String(), vulkan ? "Vulkan" : vendor.String());
	fRendererView->SetText(text);

	text.SetToFormat("%.1f frames per second, %.2f ms per frame, %s %s",
		fps, frame / 1000.0, vulkan ? "Vulkan" : "GL", version.String());
	fRateView->SetText(text);
}


bool
CubeWindow::QuitRequested()
{
	_StopRenderer();
	return BWindow::QuitRequested();
}


int
main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--require-hardware") == 0)
			sRequireHardware = true;
		else if (strcmp(argv[i], "--vsync") == 0)
			sVerticalSync = true;
		else if (strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
			const char* renderer = argv[++i];
			if (strcmp(renderer, "opengl") == 0)
				sRendererMode = RENDERER_OPENGL;
			else if (strcmp(renderer, "vulkan") == 0)
				sRendererMode = RENDERER_VULKAN;
			else {
				fprintf(stderr, "Unknown renderer: %s\n", renderer);
				return 2;
			}
		}
		else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			char* end;
			errno = 0;
			unsigned long count = strtoul(argv[++i], &end, 10);
			if (errno || *end || count == 0 || count > 10000000) {
				fprintf(stderr, "Invalid frame count\n");
				return 2;
			}
			sFrameLimit = count;
		} else {
			fprintf(stderr, "Usage: intel_gfx_cube [--renderer opengl|vulkan] "
				"[--require-hardware] [--vsync] [--frames N]\n");
			return 2;
		}
	}
	BApplication application("application/x-vnd.IntelGfx-cube");
	CubeWindow* window = new CubeWindow();
	window->Show();
	application.Run();
	return sExitCode.load();
}
