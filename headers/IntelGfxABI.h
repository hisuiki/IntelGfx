/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_ABI_H
#define INTEL_GFX_ABI_H

#include <Drivers.h>
#include <stdint.h>

namespace IntelGfx {

static const uint32_t kABIVersion = 8;
static const uint64_t kMaxBufferSize = 64ULL * 1024 * 1024;
// What one client may hold in system memory at once. A browser compositing a
// long image-heavy page reaches a quarter of a gigabyte without doing anything
// unreasonable, and the allocation that fails past this point is not recovered
// from gracefully: Mesa reports it as an out-of-memory to its caller, and
// Firefox answers that by turning off hardware rendering for the rest of the
// session. The buffers are wired, so this is not free, but a limit low enough
// to be reached during ordinary browsing costs far more than the memory does.
static const uint64_t kClientMemoryLimit = 1024ULL * 1024 * 1024;
// Global GTT space is a scarce device resource shared with the display, so a
// client may pin far less of it than it may allocate in system memory.
static const uint64_t kClientApertureLimit = 64ULL * 1024 * 1024;
static const uint32_t kMaxBuffers = 256;
static const uint32_t kMaxContexts = 16;
static const uint64_t kVirtualAddressLimit = 1ULL << 47;

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
	kCreateContext,
	kDestroyContext,
	kBindVirtual,
	kUnbindVirtual,
	kSubmitObjects,
	kWaitRenderFence,
	kBufferLayout,
	kCacheBuffer,
	kGetTopology,
	kGpuActivity,
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
	kRenderSubmission = 1ULL << 3,
	// Native render ABI: per-open PPGTT, independent logical contexts,
	// fixed virtual bindings and synchronous multi-object submissions.
	kNativeRender = 1ULL << 4
};

// All ioctl layouts use fixed widths. No user pointers or native size_t fields.
struct Header {
	uint32_t version;
	uint32_t size;
};

struct CreateContext {
	Header header;
	uint32_t context;
	uint32_t flags;
};

struct DestroyContext {
	Header header;
	uint32_t context;
	uint32_t reserved;
};

// The caller chooses a stable PPGTT address. These bindings consume no GGTT
// aperture. Zero, overlapping, non-page-aligned and upper-half VAs are rejected.
struct BindVirtual {
	Header header;
	uint32_t handle;
	uint32_t flags;
	uint64_t address;
};

struct SubmitObjects {
	Header header;
	uint32_t context;
	uint32_t batchHandle;
	uint64_t offset;
	uint64_t length;
	uint64_t fence;
	uint32_t count;
	uint32_t flags;
	// All referenced BOs, including the batch. The driver holds the client
	// lock and their mappings until execution AND context save complete.
	uint32_t handles[kMaxBuffers];
};

static const uint32_t kMaxActivityClients = 16;
// The command streamer's timestamp counts at a fixed rate per generation;
// twelve megahertz is the generation 9 rate for the parts this driver drives.
static const uint32_t kGpuTimestampHz = 12000000;

// What the GPU has spent its time on, taken from the timestamp the engine
// keeps inside each logical context and saves back with it. That is time the
// hardware really spent running that context, which is what a utilisation
// figure should be made of; it does not depend on the render domain's power
// state, and so does not become meaningless when the GPU stops entering RC6.
struct ActivityClient {
	int32_t team;
	uint32_t contexts;
	uint64_t ticks;
};

struct GpuActivity {
	Header header;
	uint32_t timestampHz;
	uint32_t count;
	uint64_t totalTicks;		// every context the device has ever run
	uint64_t reserved;
	ActivityClient clients[kMaxActivityClients];
};

// How many shaders the device has and which of them are fused on, decoded
// from the fuse registers by the driver. A client cannot read those registers
// for itself: they are in a forcewake domain, so they answer zero whenever the
// GPU is powered down, which is exactly the state a client that has not yet
// drawn anything finds it in. Reading them under forcewake is the driver's
// job, and it does it once, while it is already holding the domain awake.
struct Topology {
	Header header;
	uint32_t maxSlices;
	uint32_t maxSubslices;
	uint32_t maxEusPerSubslice;
	uint32_t sliceMask;
	uint32_t subsliceMask[3];	// one per slice
	uint32_t euMask[3];			// four packed eight bit masks, one per slice
};

enum Tiling { kLinear = 0, kXTile = 1, kYTile = 2 };
struct BufferLayout {
	Header header;
	uint32_t handle;
	uint32_t tiling;
	uint32_t stride;
	uint32_t reserved;
};

// CPU mappings are cached. Flush/invalidate this byte range before crossing
// CPU/GPU ownership; tiled mappings contain raw tiles, never a fence detile.
struct CacheBuffer {
	Header header;
	uint32_t handle;
	uint32_t reserved;
	uint64_t offset;
	uint64_t length;
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
static_assert(sizeof(ActivityClient) == 16, "ActivityClient ABI");
static_assert(sizeof(GpuActivity) == 288, "GpuActivity ABI");
static_assert(sizeof(Topology) == 48, "Topology ABI");
static_assert(sizeof(CreateContext) == 16, "CreateContext ABI");
static_assert(sizeof(DestroyContext) == 16, "DestroyContext ABI");
static_assert(sizeof(BindVirtual) == 24, "BindVirtual ABI");
static_assert(sizeof(SubmitObjects) == 1072, "SubmitObjects ABI");
static_assert(sizeof(BufferLayout) == 24, "BufferLayout ABI");
static_assert(sizeof(CacheBuffer) == 32, "CacheBuffer ABI");
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
