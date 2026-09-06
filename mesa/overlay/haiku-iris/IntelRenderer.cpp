/*
 * Copyright 2006-2012, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jérôme Duval, korli@users.berlios.de
 *		Philippe Houdoin, philippe.houdoin@free.fr
 *		Artur Wyszynski, harakash@gmail.com
 *		Alexander von Gluck IV, kallisti5@unixzen.com
 */


#include "IntelRenderer.h"

#include <Autolock.h>
#include <interface/DirectWindowPrivate.h>
#include <interface/ColorConversion.h>
#include <GraphicsDefs.h>
#include <Screen.h>
#include <stdio.h>
#include <sys/time.h>
#include <new>


#ifdef DEBUG
#	define TRACE(x...) printf("IntelRenderer: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
#	define CALLED()
#endif
#define ERROR(x...)	printf("IntelRenderer: " x)


extern const char* color_space_name(color_space space);


extern "C" _EXPORT BGLRenderer*
instantiate_gl_renderer(BGLView *view, ulong opts)
{
	IntelRenderer* renderer = new(std::nothrow) IntelRenderer(view, opts);
	if (renderer != NULL && renderer->InitCheck() != B_OK) {
		delete renderer;
		return NULL;
	}
	return renderer;
}

struct RasBuffer
{
	int32 width, height, stride;
	int32 orgX, orgY;
	uint8 *colors;
	color_space pixel_format;
	int32 pixel_size;	

	RasBuffer(int32 width, int32 height, int32 stride, int32 orgX, int32 orgY, void *colors):
		width(width), height(height), stride(stride), orgX(orgX), orgY(orgY),
		colors((uint8*)colors)
	{}

	RasBuffer(BBitmap *bmp)
	{
		width  = bmp->Bounds().IntegerWidth()  + 1;
		height = bmp->Bounds().IntegerHeight() + 1;
		stride = bmp->BytesPerRow();
		orgX   = 0;
		orgY   = 0;
		pixel_format = bmp->ColorSpace();
		pixel_size = stride / width;
		colors = (uint8*)bmp->Bits();
	}

	RasBuffer(direct_buffer_info *info)
	{
		width  = 0x7fffffff;
		height = 0x7fffffff;
		stride = info->bytes_per_row;
		orgX   = 0;
		orgY   = 0;
		pixel_format = info->pixel_format;
		pixel_size = info->bits_per_pixel / 8;
		colors = (uint8*)info->bits;
	}

	void ClipSize(int32 x, int32 y, int32 w, int32 h)
	{
		if (x < 0) {w += x; x = 0;}
		if (y < 0) {h += y; y = 0;}
		if (x + w > width) {w = width  - x;}
		if (y + h > height) {h = height - y;}
		if ((w > 0) && (h > 0)) {
			colors += (y * stride) + (x * pixel_size);
			width  = w;
			height = h;
		} else {
			width = 0; height = 0; colors = NULL;
		}
		if (x + orgX > 0) {orgX += x;} else {orgX = 0;}
		if (y + orgY > 0) {orgY += y;} else {orgY = 0;}
	}

	void ClipRect(int32 l, int32 t, int32 r, int32 b)
	{
		ClipSize(l, t, r - l, b - t);
	}

	void Shift(int32 dx, int32 dy)
	{
		orgX += dx;
		orgY += dy;
	}

	void Blit(RasBuffer src)
	{
		RasBuffer dst = *this;
		int32 x, y;
		x = src.orgX - orgX;
		y = src.orgY - orgY;
		dst.ClipSize(x, y, src.width, src.height);
		src.ClipSize(-x, -y, width, height);
		for (; dst.height > 0; dst.height--) {
			if (src.pixel_format == dst.pixel_format) {
				memcpy(dst.colors, src.colors, src.width * src.pixel_size);
			} else {
				BPrivate::ConvertBits(src.colors, dst.colors, src.stride, dst.stride,
					src.stride, dst.stride, src.pixel_format, dst.pixel_format, src.width, 1);
			}
			dst.colors += dst.stride;
			src.colors += src.stride;
		}
	}
};

IntelRenderer::IntelRenderer(BGLView *view, ulong options)
	:
	BGLRenderer(view, options),
	fContextObj(NULL),
	fContextID(-1),
	fDirectModeEnabled(false),
	fInfo(NULL),
	fInfoLocker("info locker"),
	fOptions(options),
	fColorSpace(B_NO_COLOR_SPACE)
{
	CALLED();

	// From here the window's thread may call Draw at any time, so nothing
	// below may be reached with a half built context object.
	fContextObj = new GalliumContext(options);

	BRect b = view->Bounds();
	fColorSpace = BScreen(view->Window()).ColorSpace();
	TRACE("%s: Colorspace:\t%s\n", __func__, color_space_name(fColorSpace));

	fWidth = (GLint)b.IntegerWidth();
	fHeight = (GLint)b.IntegerHeight();

	// Initialize the first "Haiku Intel GL Pipe" context
	fContextID = fContextObj->CreateContext(this);

	if (fContextID < 0)
		ERROR("%s: There was an error creating the context!\n", __func__);

	if (fContextID >= 0 && !fContextObj->GetCurrentContext())
		LockGL();
}


IntelRenderer::~IntelRenderer()
{
	CALLED();

	if (fContextObj)
		delete fContextObj;
}


void
IntelRenderer::LockGL()
{
//	CALLED();
	BGLRenderer::LockGL();

	color_space cs = BScreen(GLView()->Window()).ColorSpace();

	{
		BAutolock lock(fInfoLocker);
		if (fDirectModeEnabled && fInfo != NULL) {
			fWidth = fInfo->window_bounds.right - fInfo->window_bounds.left;
			fHeight = fInfo->window_bounds.bottom - fInfo->window_bounds.top;
		}

		fContextObj->Validate(fWidth, fHeight);
		fColorSpace = cs;
	}

	// do not hold fInfoLocker here to avoid deadlock
	fContextObj->SetCurrentContext(true, fContextID);
}


void
IntelRenderer::UnlockGL()
{
//	CALLED();
	if ((fOptions & BGL_DOUBLE) == 0) {
		SwapBuffers();
	}
	fContextObj->SetCurrentContext(false, fContextID);
	BGLRenderer::UnlockGL();
}


void
IntelRenderer::Display(BBitmap *bitmap, BRect *updateRect)
{
//	CALLED();

	if (!fDirectModeEnabled) {
		// TODO: avoid timeout
		if (GLView()->LockLooperWithTimeout(1000) == B_OK) {
			GLView()->DrawBitmap(bitmap, B_ORIGIN);
			GLView()->UnlockLooper();
		}
	} else {
		BAutolock lock(fInfoLocker);
		if (fInfo != NULL) {
			RasBuffer srcBuf(bitmap);
			RasBuffer dstBuf(fInfo);
			for (uint32 i = 0; i < fInfo->clip_list_count; i++) {
				clipping_rect *clip = &fInfo->clip_list[i];
				RasBuffer dstClip = dstBuf;
				dstClip.ClipRect(clip->left, clip->top, clip->right + 1, clip->bottom + 1);
				dstClip.Shift(-fInfo->window_bounds.left, -fInfo->window_bounds.top);
				dstClip.Blit(srcBuf);
			}
		}
	}
}


void
IntelRenderer::SwapBuffers(bool vsync)
{
	if (fContextObj == NULL || fContextID < 0)
		return;
	BScreen screen(GLView()->Window());
	fContextObj->SwapBuffers(fContextID);
	fContextObj->Validate(fWidth, fHeight);
	if (vsync)
		screen.WaitForRetrace();
}

void
IntelRenderer::Draw(BRect updateRect)
{
//	CALLED();
	// The window's thread draws the view whenever it is exposed, including
	// before this renderer finished being built and after it has begun being
	// taken apart.
	if (fContextObj == NULL || fContextID < 0)
		return;
	fContextObj->Draw(fContextID, updateRect);
}


status_t
IntelRenderer::CopyPixelsOut(BPoint location, BBitmap *bitmap)
{
	CALLED();

	// TODO: implement
	return B_ERROR;
}


status_t
IntelRenderer::CopyPixelsIn(BBitmap *bitmap, BPoint location)
{
	CALLED();

	// TODO: implement
	return B_ERROR;
}


void
IntelRenderer::EnableDirectMode(bool enabled)
{
	fDirectModeEnabled = enabled;
}


void
IntelRenderer::DirectConnected(direct_buffer_info *info)
{
//	CALLED();
	BAutolock lock(fInfoLocker);
	if (info) {
		if (!fInfo) {
			fInfo = (direct_buffer_info *)calloc(1,
				DIRECT_BUFFER_INFO_AREA_SIZE);
		}
		memcpy(fInfo, info, DIRECT_BUFFER_INFO_AREA_SIZE);
	} else if (fInfo) {
		free(fInfo);
		fInfo = NULL;
	}
}


void
IntelRenderer::FrameResized(float width, float height)
{
	TRACE("%s: %f x %f\n", __func__, width, height);

	BAutolock lock(fInfoLocker);
	fWidth = (GLuint)width;
	fHeight = (GLuint)height;
}
