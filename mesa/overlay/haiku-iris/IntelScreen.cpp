/* SPDX-License-Identifier: MIT */
#include "IntelScreen.h"
#include "IntelGfxABI.h"
#include <Bitmap.h>
#include <Directory.h>
#include <Path.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "sw/hgl/hgl_sw_winsys.h"
extern "C" struct pipe_screen* iris_screen_create(int, const struct pipe_screen_config*);

static void
present(struct pipe_screen*, struct pipe_context* pipe,
 struct pipe_resource* resource, unsigned level, unsigned layer,
 void* privateContext, struct pipe_box*)
{
 if (!pipe || !privateContext || !resource) return;
 struct pipe_transfer* transfer = NULL;
 void* pixels = pipe_texture_map(pipe, resource, level, layer, PIPE_MAP_READ,
  0, 0, resource->width0, resource->height0, &transfer);
 if (!pixels) { fprintf(stderr, "Intel Gallium: framebuffer readback failed\n"); return; }
 BBitmap bitmap(BRect(0, 0, resource->width0 - 1, resource->height0 - 1), B_RGB32);
 if (bitmap.InitCheck() == B_OK) {
  util_format_translate(PIPE_FORMAT_B8G8R8X8_UNORM, bitmap.Bits(),
   bitmap.BytesPerRow(), 0, 0, resource->format, pixels, transfer->stride,
   0, 0, resource->width0, resource->height0);
  ((HGLWinsysContext*)privateContext)->Display(&bitmap, NULL);
 }
 pipe_texture_unmap(pipe, transfer);
}

static int
open_device(const char* path)
{
 int fd = open(path, O_RDWR | O_CLOEXEC);
 if (fd < 0) return -1;
 auto info = IntelGfx::Request<IntelGfx::DeviceInfo>();
 if (ioctl(fd, IntelGfx::kGetInfo, &info, sizeof(info)) < 0
  || info.header.version != IntelGfx::kABIVersion
  || !(info.capabilities & IntelGfx::kNativeRender)) {
  close(fd); return -1;
 }
 return fd;
}

struct pipe_screen*
intel_haiku_screen_create()
{
 int fd = -1;
 const char* overridePath = getenv("INTEL_GFX_DEVICE");
 if (overridePath) fd = open_device(overridePath);
 else {
  BDirectory devices("/dev/graphics"); BEntry entry;
  while (devices.GetNextEntry(&entry) == B_OK) {
   BPath path;
   if (entry.GetPath(&path) != B_OK
    || strncmp(path.Leaf(), "intel_extreme_", 14) != 0)
    continue;
   if ((fd = open_device(path.Path())) >= 0)
    break;
  }
 }
 if (fd < 0) return NULL;
 struct pipe_screen* screen = iris_screen_create(fd, NULL);
 if (!screen) { close(fd); return NULL; }
 screen->flush_frontbuffer = present;
 return screen;
}
