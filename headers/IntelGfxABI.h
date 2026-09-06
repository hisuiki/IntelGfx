/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_ABI_H
#define INTEL_GFX_ABI_H

#include <Drivers.h>
#include <stdint.h>

namespace IntelGfx {

static const uint32_t kABIVersion = 5;
static const uint64_t kMaxBufferSize = 64ULL * 1024 * 1024;
static const uint64_t kClientMemoryLimit = 256ULL * 1024 * 1024;
// Global GTT space is a scarce device resource shared with the display, so a
// client may pin far less of it than it may allocate in system memory.
static const uint64_t kClientApertureLimit = 64ULL * 1024 * 1024;
static const uint32_t kMaxBuffers = 256;

enum Operation {
	kGetInfo = B_DEVICE_OP_CODES_END + 0x4900,
	kCreateBuffer,
	kCloseBuffer,
	kBindBuffer,
	kUnbindBuffer,
	kSubmit,
	kWaitFence,
	kEngineStatus,
	kReadRegister,
	kFramebuffer,
	kDisplayStatus,
	// Keep last: the driver routes everything below this to the new
	// interface, so adding an operation above needs no change there.
	kOperationsEnd
};

// Flags for a submission. Commands that draw need the render engine; simple
// memory work is better left on the blitter, which nothing else is using.
enum SubmitFlags {
	kUseRenderEngine = 1 << 0
};

enum Capability {
	kDisplay = 1ULL << 0,
	kCpuBuffers = 1ULL << 1,
	kGpuVirtualMemory = 1ULL << 2,
	kRenderSubmission = 1ULL << 3
};

// All ioctl layouts use fixed widths. No user pointers or native size_t fields.
struct Header {
	uint32_t version;
	uint32_t size;
};

struct DeviceInfo {
	Header header;
	uint64_t capabilities;
	uint64_t maxBufferSize;
	// The range of the device's global address space this driver hands out.
	// Every graphics address falls inside it. Both are zero when
	// kGpuVirtualMemory is not reported.
	uint64_t graphicsAddressBase;
	uint64_t graphicsAddressSize;
	uint16_t vendor;
	uint16_t device;
	uint8_t revision;
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
	uint32_t graphicsVersion;
	uint32_t reserved;
};

struct CreateBuffer {
	Header header;
	uint64_t size;
	uint32_t flags;
	uint32_t handle;
	int32_t area;
	uint32_t reserved;
};

struct CloseBuffer {
	Header header;
	uint32_t handle;
	uint32_t reserved;
};

// Pins a buffer's pages into the global GTT. The graphics address is a byte
// offset from the start of the aperture, which is the form the display engine
// and the render command streamer consume. It is page aligned and stays valid
// until the buffer is unbound or closed; it is not stable across rebinds.
struct BindBuffer {
	Header header;
	uint32_t handle;
	uint32_t flags;
	uint64_t graphicsAddress;
};

struct UnbindBuffer {
	Header header;
	uint32_t handle;
	uint32_t reserved;
};

// Runs the commands in a bound buffer on the GPU. The buffer's graphics
// address is where the engine starts, and the commands must end in
// MI_BATCH_BUFFER_END. The fence returned is complete once everything the
// commands wrote is visible.
struct SubmitBatch {
	Header header;
	uint32_t handle;
	uint32_t flags;
	uint64_t offset;
	uint64_t length;
	uint64_t fence;
};

struct WaitFence {
	Header header;
	uint64_t fence;
	uint64_t timeout;			// microseconds
};

// What the engine looks like from outside: enough to tell a context that
// never started from one that ran and wrote nothing.
struct EngineStatus {
	Header header;
	uint32_t ringHead;
	uint32_t ringTail;
	uint32_t ringStart;
	uint32_t ringControl;
	uint32_t activeHead;
	uint32_t instructionHeader;
	uint32_t errorIdentity;
	uint32_t miMode;
	uint32_t mode;
	uint32_t execlistStatusLow;
	uint32_t execlistStatusHigh;
	uint32_t statusPointer;
	uint32_t interruptStatus;
	uint32_t hardwareStatusAddress;
	uint32_t contextRingHead;		// as the engine saved them back
	uint32_t contextRingTail;
	uint32_t contextRingStart;
	uint32_t contextRingControl;
	uint32_t contextControl;
	uint32_t contextChanged;		// dwords the engine rewrote in the image
	uint32_t contextFirstChange;	// where the first of them is
	uint32_t ringFirstDword;		// what the ring holds, as memory sees it
	uint32_t fence;					// what the fence page holds
	uint32_t submitted;				// the sequence number last submitted
	uint32_t statusBuffer[12];		// context switch events
};

// Reads one memory mapped register. Diagnostics only, and read only: the
// display driver and this interface share one device, and writing behind the
// other's back is how a working display gets lost.
struct ReadRegister {
	Header header;
	uint32_t offset;
	uint32_t value;
};

// The framebuffer the display is scanning out, as the GPU addresses it. It
// lives in the part of the address space the display driver allocates, below
// everything this interface hands out, but it is addressable all the same.
struct Framebuffer {
	Header header;
	uint64_t address;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint32_t bitsPerPixel;
};

// Whether the display is interrupting, and whether anything asked it to.
// A frame count that moves while the vertical blank count stands still means
// the display is scanning out but its interrupt never arrives.
struct DisplayStatus {
	Header header;
	uint64_t vblankCount;
	uint32_t masterInterrupt;
	uint32_t pipeInterruptEnable[3];
	uint32_t pipeInterruptMask[3];
	uint32_t frameCount[3];
	uint32_t reserved[2];
};

template<typename T> inline T Request()
{
	T request = {};
	request.header.version = kABIVersion;
	request.header.size = sizeof(T);
	return request;
}

static_assert(sizeof(DeviceInfo) == 56, "DeviceInfo ABI");
static_assert(sizeof(CreateBuffer) == 32, "CreateBuffer ABI");
static_assert(sizeof(CloseBuffer) == 16, "CloseBuffer ABI");
static_assert(sizeof(BindBuffer) == 24, "BindBuffer ABI");
static_assert(sizeof(UnbindBuffer) == 16, "UnbindBuffer ABI");
static_assert(sizeof(SubmitBatch) == 40, "SubmitBatch ABI");
static_assert(sizeof(WaitFence) == 24, "WaitFence ABI");
static_assert(sizeof(EngineStatus) == 152, "EngineStatus ABI");
static_assert(sizeof(ReadRegister) == 16, "ReadRegister ABI");
static_assert(sizeof(Framebuffer) == 32, "Framebuffer ABI");
static_assert(sizeof(DisplayStatus) == 64, "DisplayStatus ABI");

} // namespace IntelGfx
#endif
