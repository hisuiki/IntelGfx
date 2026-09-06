/* SPDX-License-Identifier: MIT */
// A rotating cube, for finding out what draws it. The interesting part is
// not the cube: it is the renderer the GL kit gives us and how fast it goes,
// which is how the difference between a software rasteriser and a driver
// that uses this hardware will show itself.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <GLView.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <Window.h>

#include <GL/gl.h>
#include <GL/glu.h>


static const uint32 kStatistics = 'stat';
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
			bool				fRunning;
			// The drawing thread may not ask the view about itself: that
			// needs the window locked, and this thread is not the one that
			// holds it. The size it draws to is recorded here instead, by
			// the thread that is allowed to know it.
			float				fWidth;
			float				fHeight;
			BMessenger			fWindow;
			BString				fRenderer;
			BString				fVendor;
			BString				fVersion;
};


CubeView::CubeView()
	:
	BGLView(BRect(0, 0, 1, 1), "cube", B_FOLLOW_ALL_SIDES, 0,
		BGL_RGB | BGL_DOUBLE | BGL_DEPTH),
	fThread(-1),
	fRunning(false),
	fWidth(1.0f),
	fHeight(1.0f)
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
	float width = fWidth;
	float height = fHeight;
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

	glBegin(GL_QUADS);
	for (int face = 0; face < 6; face++) {
		glColor3fv(kColours[face]);
		for (int vertex = 0; vertex < 4; vertex++)
			glVertex3fv(&kFaces[face][vertex * 3]);
	}
	glEnd();
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

	float angle = 0.0f;
	uint32 frames = 0;
	bigtime_t reportedAt = system_time();

	while (fRunning) {
		bigtime_t frameStart = system_time();

		LockGL();
		_Draw(angle);
		SwapBuffers();
		UnlockGL();

		angle += 1.0f;
		if (angle >= 360.0f)
			angle -= 360.0f;
		frames++;

		bigtime_t now = system_time();
		if (now - reportedAt >= kReportInterval) {
			float seconds = (now - reportedAt) / 1000000.0f;
			float fps = frames / seconds;
			BMessage message(kStatistics);
			message.AddFloat("fps", fps);
			message.AddInt64("frame", now - frameStart);
			message.AddString("renderer", fRenderer);
			message.AddString("vendor", fVendor);
			message.AddString("version", fVersion);
			fWindow.SendMessage(&message);

			printf("%.1f frames per second, %.2f ms per frame\n", fps,
				(now - frameStart) / 1000.0);
			fflush(stdout);

			frames = 0;
			reportedAt = now;
		}
	}
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
	fWidth = width + 1;
	fHeight = height + 1;
}


void
CubeView::_Start()
{
	if (fThread >= 0)
		return;
	fRunning = true;
	fThread = spawn_thread(_ThreadEntry, "cube", B_NORMAL_PRIORITY, this);
	if (fThread >= 0)
		resume_thread(fThread);
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
			CubeView*			fView;
			BStringView*		fRendererView;
			BStringView*		fRateView;
};


CubeWindow::CubeWindow()
	:
	BWindow(BRect(80, 80, 720, 560), "GPU cube", B_TITLED_WINDOW,
		B_QUIT_ON_WINDOW_CLOSE | B_AUTO_UPDATE_SIZE_LIMITS)
{
	fView = new CubeView();
	fRendererView = new BStringView("renderer", "Asking the GL kit…");
	fRateView = new BStringView("rate", "Measuring…");

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fView)
		.AddGroup(B_VERTICAL, 2)
			.SetInsets(8, 6, 8, 6)
			.Add(fRendererView)
			.Add(fRateView)
			.End();
}


void
CubeWindow::MessageReceived(BMessage* message)
{
	if (message->what != kStatistics) {
		BWindow::MessageReceived(message);
		return;
	}

	BString renderer, vendor, version;
	float fps = 0;
	int64 frame = 0;
	message->FindString("renderer", &renderer);
	message->FindString("vendor", &vendor);
	message->FindString("version", &version);
	message->FindFloat("fps", &fps);
	message->FindInt64("frame", &frame);

	// A renderer that names a software rasteriser is worth saying out loud:
	// it is the difference this driver is meant to remove.
	bool software = renderer.IFindFirst("llvmpipe") >= 0
		|| renderer.IFindFirst("softpipe") >= 0
		|| renderer.IFindFirst("swrast") >= 0
		|| renderer.IFindFirst("software") >= 0;

	BString text;
	text.SetToFormat("%s — %s (%s)", software ? "Software rasteriser"
		: "Hardware", renderer.String(), vendor.String());
	fRendererView->SetText(text);

	text.SetToFormat("%.1f frames per second, %.2f ms per frame, GL %s",
		fps, frame / 1000.0, version.String());
	fRateView->SetText(text);
}


bool
CubeWindow::QuitRequested()
{
	fView->Stop();
	return BWindow::QuitRequested();
}


int
main()
{
	BApplication application("application/x-vnd.IntelGfx-cube");
	CubeWindow* window = new CubeWindow();
	window->Show();
	application.Run();
	return 0;
}
