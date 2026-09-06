/* SPDX-License-Identifier: MIT */
#include "RenderEngine.h"

#include "GpuHardware.h"
#include "Cache.h"

#include <string.h>
#include <util/AutoLock.h>

namespace IntelGfx {

static const uint32 kRingSize = 16 * 1024;
static const uint32 kContextSize = 4 * B_PAGE_SIZE;
static const uint32 kStateOffset = B_PAGE_SIZE;	// the register image follows
												// the context's status page
static const uint32 kFenceOffset = 0x100;		// eight byte aligned, bit 5 clear
static const bigtime_t kForcewakeTimeout = 50000;
static const bigtime_t kIdleTimeout = 1000000;
static const bigtime_t kSpinTimeout = 2000;

// Index of the values this driver fills in, in dwords into the register
// image. They follow from the layout below and match i915's names for them.
static const uint32 kStateContextControl = 0x02 + 1;
static const uint32 kStateRingHead = 0x04 + 1;
static const uint32 kStateRingTail = 0x06 + 1;
static const uint32 kStateRingStart = 0x08 + 1;
static const uint32 kStateRingControl = 0x0a + 1;
static const uint32 kStateBatchBufferState = 0x10 + 1;
static const uint32 kStateTimestamp = 0x22 + 1;
static const uint32 kStatePageDirectory0Upper = 0x30 + 1;
static const uint32 kStatePageDirectory0Lower = 0x32 + 1;
static const uint32 kStateMiMode = 0x54;

// The register image is a batch of MI_LOAD_REGISTER_IMM commands whose exact
// shape the hardware dictates, differing per generation and engine class.
// This is the layout Linux's i915 driver uses for the generation 9 engines
// that are not the render engine, in the same encoding: a byte with bit 7 set
// skips that many dwords, any other byte starts a load of that many registers
// (bit 6 asking for the posted flag), and the register offsets that follow
// are stored shifted down by two, with bit 7 marking one that needs a second
// byte for its low bits.
#define NOP(count)		(0x80 | (count))
#define LRI(count, posted)	(((posted) << 6) | (count))
#define REG(offset)		((offset) >> 2)
#define REG16(offset)		(((offset) >> 9) | 0x80), (((offset) >> 2) & 0x7f)
#define POSTED			1

static const uint8 kEngineStateLayout[] = {
	NOP(1),
	LRI(14, POSTED),
	REG16(0x244), REG(0x034), REG(0x030), REG(0x038), REG(0x03c),
	REG(0x168), REG(0x140), REG(0x110), REG(0x11c), REG(0x114),
	REG(0x118), REG(0x1c0), REG(0x1c4), REG(0x1c8),

	NOP(3),
	LRI(9, POSTED),
	REG16(0x3a8), REG16(0x28c), REG16(0x288), REG16(0x284), REG16(0x280),
	REG16(0x27c), REG16(0x278), REG16(0x274), REG16(0x270),

	NOP(13),
	LRI(1, POSTED),
	REG16(0x200),

	NOP(13),
	LRI(44, POSTED),
	REG(0x028), REG(0x09c), REG(0x0c0), REG(0x178), REG(0x17c),
	REG16(0x358), REG(0x170), REG(0x150), REG(0x154), REG(0x158),
	REG16(0x41c), REG16(0x600), REG16(0x604), REG16(0x608), REG16(0x60c),
	REG16(0x610), REG16(0x614), REG16(0x618), REG16(0x61c), REG16(0x620),
	REG16(0x624), REG16(0x628), REG16(0x62c), REG16(0x630), REG16(0x634),
	REG16(0x638), REG16(0x63c), REG16(0x640), REG16(0x644), REG16(0x648),
	REG16(0x64c), REG16(0x650), REG16(0x654), REG16(0x658), REG16(0x65c),
	REG16(0x660), REG16(0x664), REG16(0x668), REG16(0x66c), REG16(0x670),
	REG16(0x674), REG16(0x678), REG16(0x67c), REG(0x068),

	0
};

// The render engine's image, which differs from the one above in a single
// block: where the other engines carry a register of their own, this one
// carries the power and clock state that says which slices a context runs on.
static const uint8 kRenderStateLayout[] = {
	NOP(1),
	LRI(14, POSTED),
	REG16(0x244), REG(0x034), REG(0x030), REG(0x038), REG(0x03c),
	REG(0x168), REG(0x140), REG(0x110), REG(0x11c), REG(0x114),
	REG(0x118), REG(0x1c0), REG(0x1c4), REG(0x1c8),

	NOP(3),
	LRI(9, POSTED),
	REG16(0x3a8), REG16(0x28c), REG16(0x288), REG16(0x284), REG16(0x280),
	REG16(0x27c), REG16(0x278), REG16(0x274), REG16(0x270),

	NOP(13),
	LRI(1, 0),
	REG(0x0c8),			// GEN8_R_PWR_CLK_STATE

	NOP(13),
	LRI(44, POSTED),
	REG(0x028), REG(0x09c), REG(0x0c0), REG(0x178), REG(0x17c),
	REG16(0x358), REG(0x170), REG(0x150), REG(0x154), REG(0x158),
	REG16(0x41c), REG16(0x600), REG16(0x604), REG16(0x608), REG16(0x60c),
	REG16(0x610), REG16(0x614), REG16(0x618), REG16(0x61c), REG16(0x620),
	REG16(0x624), REG16(0x628), REG16(0x62c), REG16(0x630), REG16(0x634),
	REG16(0x638), REG16(0x63c), REG16(0x640), REG16(0x644), REG16(0x648),
	REG16(0x64c), REG16(0x650), REG16(0x654), REG16(0x658), REG16(0x65c),
	REG16(0x660), REG16(0x664), REG16(0x668), REG16(0x66c), REG16(0x670),
	REG16(0x674), REG16(0x678), REG16(0x67c), REG(0x068),

	0
};

#undef NOP
#undef LRI
#undef REG
#undef REG16
#undef POSTED


// The two engines this driver can drive. They differ in where their
// registers are, which forcewake domain keeps them awake, the shape of their
// context image, and how much room that image needs.
const EngineDescriptor kBlitterEngine = {
	"blitter", kBlitterEngineBase, kForcewakeBlitter, kForcewakeBlitterAck,
	kEngineStateLayout, 4 * B_PAGE_SIZE, false
};

const EngineDescriptor kRenderEngine = {
	"render", kRenderEngineBase, kForcewakeRender, kForcewakeRenderAck,
	kRenderStateLayout, 24 * B_PAGE_SIZE, true
};


static void
WriteStateLayout(uint32* state, uint32 base, const uint8* data)
{
	uint32* registers = state;

	while (*data != 0) {
		if ((*data & 0x80) != 0) {
			registers += *data++ & ~0x80;
			continue;
		}

		uint32 count = *data & 0x3f;
		bool posted = (*data >> 6) != 0;
		data++;

		*registers = kMiLoadRegisterImmediate | (2 * count - 1);
		if (posted)
			*registers |= kMiLoadRegisterPosted;
		registers++;

		while (count-- > 0) {
			uint32 offset = 0;
			uint8 byte;
			do {
				byte = *data++;
				offset <<= 7;
				offset |= byte & ~0x80;
			} while ((byte & 0x80) != 0);

			registers[0] = base + (offset << 2);
			registers += 2;
		}
	}
}


RenderEngine::RenderEngine()
	:
	fRegisters(0),
	fEngine(&kBlitterEngine),
	fReady(false),
	fFaulted(false),
	fScheduler(this),
	fAddressSpace(&fPageTables),
	fRegisterState(NULL),
	fRingSize(kRingSize),
	fRingTail(0),
	fNextSeqno(1),
	fGpuTicks(0),
	fTotalGpuTicks(0),
	fLastTimestamp(0),
	fDevice(0),
	fRevision(0)
{
	mutex_init(&fLock, "intel_gfx engine");
}


void
RenderEngine::_InitRenderWorkarounds()
{
	if (!fEngine->usesPipeControl)
		return;

	// Gen9 GT workarounds required before a 3D context runs. These mirror
	// i915's SKL/KBL programming: coherent LLC compression settings, safe
	// HDC invalidation, and clock/decompression controls.
	_Write(0x4090, _Read(0x4090) | (1 << 25) | (1 << 8));
	_Write(0x4ddc, _Read(0x4ddc) | (1u << 31) | (1 << 27));
	_Write(0x940c, _Read(0x940c) | (1 << 14));
	bool kabyLake = (fDevice & 0xff00) == 0x5900;
	bool skyLakeH0 = (fDevice & 0xff00) == 0x1900 && fRevision >= 7;
	if (kabyLake || skyLakeH0)
		_Write(0x4ab0, _Read(0x4ab0) | (1 << 18));
	if (kabyLake && fRevision <= 1)
		_Write(0x4ab8, _Read(0x4ab8) | (1 << 28));

	// Render-engine workarounds live outside the logical context image.
	_Write(0x20e0, (1 << 30) | (1 << 14));
	_Write(0xb004, _Read(0xb004) | (1 << 7));
	_Write(0x20d4, (1 << 18) | (1 << 2));
	_Write(0xb11c, _Read(0xb11c) | (1 << 2));
	_Write(0xb118, _Read(0xb118) | (1 << 21));

	// Iris programs these context registers from its batches. The command
	// parser permits them only when the render engine's nonprivileged list
	// names them explicitly.
	static const uint32 kNonPrivileged[] = {
		0x2248, 0x2580, 0x7304, 0x7014, 0xb118
	};
	for (uint32 i = 0; i < 12; i++) {
		uint32 allowed = i < B_COUNT_OF(kNonPrivileged)
			? kNonPrivileged[i] : fEngine->base + 0x94;
		_Write(fEngine->base + 0x4d0 + i * 4, allowed);
	}
}


static inline void
AppendMaskedRegister(uint32* ring, uint32& at, uint32 reg, uint32 mask,
	uint32 value)
{
	ring[at++] = kMiLoadRegisterImmediate | 1;
	ring[at++] = reg;
	ring[at++] = (mask << 16) | value;
}


void
RenderEngine::_AppendRenderWorkarounds(uint32* ring, uint32& at) const
{
	// Context workarounds have to execute after restore and before client
	// commands. Masked writes preserve every unrelated register bit.
	uint32 commonSlice = 1 << 13;
	if ((fDevice & 0xff00) == 0x5900 && fRevision >= 2)
		commonSlice |= 1 << 8;
	AppendMaskedRegister(ring, at, 0x7014, commonSlice, commonSlice);
	AppendMaskedRegister(ring, at, 0xe194,
		(1 << 8) | (1 << 4) | (1 << 2),
		(1 << 8) | (1 << 4) | (1 << 2));
	AppendMaskedRegister(ring, at, 0xe4f0, (1 << 15) | (1 << 8),
		(1 << 15) | (1 << 8));
	AppendMaskedRegister(ring, at, 0x7004, (1 << 6) | (1 << 1),
		(1 << 6) | (1 << 1));
	AppendMaskedRegister(ring, at, 0xe188, 1 << 3, 0);
	AppendMaskedRegister(ring, at, 0x7300,
		(1 << 15) | (1 << 5) | (1 << 4),
		(1 << 15) | (1 << 5) | (1 << 4));
	AppendMaskedRegister(ring, at, 0xe184, 1 << 1, 1 << 1);
	AppendMaskedRegister(ring, at, 0xe180, 1 << 13, 1 << 13);
	AppendMaskedRegister(ring, at, 0x2580, 7, 4);
	if ((fDevice & 0xff00) == 0x5900)
		AppendMaskedRegister(ring, at, 0xe100, 1 << 4, 1 << 4);
}


RenderEngine::~RenderEngine()
{
	if (fReady && fScheduler == this) {
		// Leave the engine as it was found: no execution list, and nothing
		// pointing at memory that is about to be freed.
		if (_Forcewake(true) == B_OK) {
			_Write(fEngine->base + kRingMode, Masked(kExeclistEnable, false));
			_Forcewake(false);
		}
	}
	mutex_destroy(&fLock);
}


uint32
RenderEngine::_Read(uint32 offset) const
{
	return *(volatile uint32*)(fRegisters + offset);
}


void
RenderEngine::_Write(uint32 offset, uint32 value)
{
	*(volatile uint32*)(fRegisters + offset) = value;
}


status_t
RenderEngine::_Forcewake(bool take)
{
	_Write(fEngine->forcewake, Masked(kForcewakeKernel, take));

	bigtime_t deadline = system_time() + kForcewakeTimeout;
	while (system_time() < deadline) {
		bool awake = (_Read(fEngine->forcewakeAck) & kForcewakeKernel) != 0;
		if (awake == take)
			return B_OK;
		spin(10);
	}
	return B_TIMED_OUT;
}


void
RenderEngine::_InitContext()
{
	uint8* context = (uint8*)fContext.Address();
	memset(context, 0, fContext.Size());

	fRegisterState = (uint32*)(context + kStateOffset);
	WriteStateLayout(fRegisterState, fEngine->base, fEngine->layout);

	// Hold off the synchronous context switch, let the context be saved, and
	// ask for the first restore to be inhibited: the image starts out zeroed
	// rather than saved from a run, so there is nothing yet to restore.
	fRegisterState[kStateContextControl]
		= Masked(kContextControlInhibitSyn, true)
			| Masked(kContextControlRestoreInhibit, true)
			| Masked((1 << 1) | (1 << 2), false);
	fRegisterState[kStateRingHead] = 0;
	fRegisterState[kStateRingTail] = 0;
	fRegisterState[kStateRingStart] = (uint32)fRing.GraphicsAddress();
	fRegisterState[kStateRingControl]
		= (fRingSize - B_PAGE_SIZE) | kRingValid;
	fRegisterState[kStateBatchBufferState] = kBatchBufferPerProcessGtt;
	fRegisterState[kStateTimestamp] = 0;
	fRegisterState[kStatePageDirectory0Upper]
		= (uint32)((uint64)fAddressSpace->Root() >> 32);
	fRegisterState[kStatePageDirectory0Lower]
		= (uint32)fAddressSpace->Root();
	// The ring must not come back stopped.
	fRegisterState[kStateMiMode + 1] = Masked(kStopRing, false);
}


status_t
RenderEngine::Init(addr_t registers, GlobalGTT& gtt,
	const EngineDescriptor& engine, RenderEngine* scheduler,
	PageTables* addressSpace, uint16 device, uint8 revision)
{
	if (fReady)
		return B_BUSY;
	fEngine = &engine;
	fScheduler = scheduler != NULL ? scheduler : this;
	fAddressSpace = addressSpace != NULL ? addressSpace : &fPageTables;
	fDevice = device;
	fRevision = revision;
	if (registers == 0 || !gtt.IsValid())
		return B_NOT_SUPPORTED;

	fRegisters = registers;

	struct { BufferObject* buffer; size_t size; } objects[] = {
		{ &fStatusPage, B_PAGE_SIZE },
		{ &fFencePage, B_PAGE_SIZE },
		{ &fContext, fEngine->contextSize },
		{ &fRing, fRingSize }
	};
	for (size_t i = 0; i < B_COUNT_OF(objects); i++) {
		status_t status = objects[i].buffer->Init(objects[i].size);
		if (status != B_OK)
			return status;
		status = objects[i].buffer->Bind(gtt);
		if (status != B_OK)
			return status;
		if (objects[i].buffer->GraphicsAddress() > 0xffffffffULL) {
			// Both the context descriptor and the ring base are 32 bit.
			return B_NO_MEMORY;
		}
	}

	status_t status = fAddressSpace->IsValid() ? B_OK : fAddressSpace->Init();
	if (status != B_OK)
		return status;

	_InitContext();
	if (fEngine->usesPipeControl) {
		status = _Forcewake(true);
		if (status != B_OK) { _Forcewake(false); return status; }
		uint32 fuse = _Read(0x9120);
		uint32 slices = __builtin_popcount((fuse >> 25) & 7);
		// Gen9 RPCS must request EU enablement explicitly after power gating.
		uint32 power = (1u << 31) | (8 << 4) | 8;
		if (slices > 1)
			power |= (1 << 18) | (slices << 15);
		fRegisterState[0x42 + 1] = power;
		if (fScheduler == this)
			_InitRenderWorkarounds();
		_Forcewake(false);
	}
	FlushCpuCache(fContext.Address(), fContext.Size());
	if (fScheduler != this) {
		fReady = true;
		return B_OK;
	}

	status = _Forcewake(true);
	if (status != B_OK) {
		// Leave nothing held: an engine that never woke up must not be left
		// pinned awake either.
		_Forcewake(false);
		return status;
	}

	// Nothing here uses interrupts, and the display driver's handler knows
	// nothing about this engine, so keep its interrupts to itself.
	_Write(kGtInterruptEnable0, 0);
	_Write(kGtInterruptMask0, ~0u);
	_Write(fEngine->base + kRingHardwareStatusMask, ~0u);

	_Write(fEngine->base + kRingMode, Masked(kExeclistEnable, true));
	_Write(fEngine->base + kRingMiMode, Masked(kStopRing, false));
	_Write(fEngine->base + kRingHardwareStatusPage,
		(uint32)fStatusPage.GraphicsAddress());
	(void)_Read(fEngine->base + kRingHardwareStatusPage);

	_ReadTopology();
	_RequestMaximumFrequency();

	_Forcewake(false);

	fReady = true;
	return B_OK;
}


status_t
RenderEngine::InitClient(GlobalGTT& gtt, RenderEngine& scheduler,
	PageTables* addressSpace)
{
	return Init(scheduler.fRegisters, gtt, *scheduler.fEngine,
		scheduler.fScheduler, addressSpace, scheduler.fDevice,
		scheduler.fRevision);
}

status_t
RenderEngine::_WaitContextSaved(bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	uint32 id = (uint32)(fContext.GraphicsAddress() >> 12);
	const volatile uint32* csb = (const volatile uint32*)fStatusPage.Address()
		+ kStatusBufferIndex;
	do {
		FlushCpuCache(fStatusPage.Address(), B_PAGE_SIZE);
		for (uint32 i = 0; i < 6; i++) {
			// Gen8-10 CSB: ACTIVE_IDLE plus COMPLETE means the context's
			// image has been saved, not just that its fence command ran.
			if ((csb[2 * i] & ((1 << 3) | (1 << 4)))
					== ((1 << 3) | (1 << 4)) && csb[2 * i + 1] == id)
				return B_OK;
		}
		snooze(50);
	} while (system_time() < deadline);
	return B_TIMED_OUT;
}


status_t
RenderEngine::MapBuffer(area_id area, uint64 address)
{
	if (!fReady)
		return B_NO_INIT;
	return fAddressSpace->Map(area, address);
}


status_t
RenderEngine::MapGlobalRange(GlobalGTT& gtt, uint64 address, uint64 size)
{
	if (!fReady)
		return B_NO_INIT;

	uint64 first = address & ~((uint64)B_PAGE_SIZE - 1);
	uint64 last = (address + size + B_PAGE_SIZE - 1)
		& ~((uint64)B_PAGE_SIZE - 1);
	for (uint64 page = first; page < last; page += B_PAGE_SIZE) {
		phys_addr_t physical = 0;
		status_t status = gtt.Lookup(page, physical);
		if (status != B_OK)
			return status;
		status = fAddressSpace->MapPhysical(page, physical, B_PAGE_SIZE);
		if (status != B_OK)
			return status;
	}
	return B_OK;
}


status_t
RenderEngine::UnmapBuffer(uint64 address, uint64 size)
{
	if (!fReady)
		return B_NO_INIT;
	return fAddressSpace->Unmap(address, size);
}


uint32
RenderEngine::_Seqno()
{
	memory_read_barrier();
	return *(volatile uint32*)((uint8*)fFencePage.Address() + kFenceOffset);
}


uint64
RenderEngine::CompletedFence()
{
	if (!fReady)
		return 0;
	return _Seqno();
}


status_t
RenderEngine::_WaitSeqno(uint32 seqno, bigtime_t timeout)
{
	// A few commands finish in microseconds, so spin for a short while before
	// giving up the processor: sleeping first would make every submission
	// cost far more than the work it carries.
	bigtime_t start = system_time();
	bigtime_t deadline = start + timeout;
	bigtime_t spinUntil = start + kSpinTimeout;

	while (true) {
		if (_Seqno() >= seqno)
			return B_OK;

		bigtime_t now = system_time();
		if (now >= deadline)
			return B_TIMED_OUT;
		if (now < spinUntil)
			spin(2);
		else
			snooze(200);
	}
}


void
RenderEngine::_UpdateTail(uint32 tail)
{
	fRegisterState[kStateRingTail] = tail;
	// The context image has to be in memory before the engine is pointed at
	// it, and it is read by the GPU, not by this processor.
	FlushCpuCache(fRegisterState, B_PAGE_SIZE);
	memory_write_barrier();

	// Keep a copy to compare against once the engine has saved the context
	// back, which is how much of this it really used.
	memcpy(fStateSnapshot, fRegisterState, B_PAGE_SIZE);
}


status_t
RenderEngine::Submit(uint64 batchAddress, uint32 batchLength, uint64& _fence)
{
	if (!fReady)
		return B_NO_INIT;
	if (batchLength == 0 || (batchAddress & 0x3) != 0)
		return B_BAD_VALUE;

	MutexLocker locker(&fScheduler->fLock);

	if (fScheduler->fFaulted || fFaulted)
		return B_DEV_NOT_READY;
	// Do not wrap a 32-bit hardware timeline; create a new context instead.
	if (fNextSeqno >= 0x7fffffff)
		return B_NO_MEMORY;

	// One submission at a time: the ring is only refilled once the engine has
	// finished with what was in it.
	if (fNextSeqno > 1) {
		status_t status = _WaitSeqno(fNextSeqno - 1, kIdleTimeout);
		if (status != B_OK)
			return status;
	}

	status_t status = _Forcewake(true);
	if (status != B_OK) {
		_Forcewake(false);
		return status;
	}
	// Fetch the context image the GPU saved before changing its tail.
	FlushCpuCache(fContext.Address(), fContext.Size());
	if (fNextSeqno > 1)
		fRegisterState[kStateContextControl]
			= Masked(kContextControlRestoreInhibit, false)
				| Masked(kContextControlInhibitSyn, true)
				| Masked((1 << 1) | (1 << 2), false);

	// The batch, then whatever this engine needs to make its work visible
	// and say so. Room for the longest of the two shapes below.
	const uint32 kCommandDwords = fEngine->usesPipeControl ? 64 : 32;
	if (fRingTail + kCommandDwords * 4 > fRingSize) {
		// Pad the rest of the ring so the engine runs into the wrap cleanly.
		uint32* pad = (uint32*)((uint8*)fRing.Address() + fRingTail);
		for (uint32 i = 0; i < (fRingSize - fRingTail) / 4; i++)
			pad[i] = kMiNoop;
		fRingTail = 0;
	}

	fAddressSpace->Flush();
	uint32 seqno = fNextSeqno++;
	uint32* ring = (uint32*)((uint8*)fRing.Address() + fRingTail);
	uint32 fenceAddress = (uint32)fFencePage.GraphicsAddress() + kFenceOffset;

	// Run the client's commands, then flush and write the sequence number:
	// the flush is what makes everything the batch wrote visible before the
	// fence says it is.
	uint32 at = 0;
	if (fEngine->usesPipeControl) {
		_AppendRenderWorkarounds(ring, at);
		// Gen9 requires a null PIPE_CONTROL before VF invalidation. This
		// also invalidates translations before reusing a virtual address.
		ring[at++] = kPipeControl(6);
		for (uint32 i = 0; i < 5; i++) ring[at++] = 0;
		ring[at++] = kPipeControl(6);
		ring[at++] = kPipeControlStall | kPipeControlTlbInvalidate
			| kPipeControlQwordWrite | kPipeControlGlobalGtt
			| (1 << 11) | (1 << 10) | (1 << 4) | (1 << 3) | (1 << 2);
		ring[at++] = fenceAddress + 8; // scratch, never the completion slot
		ring[at++] = 0;
		ring[at++] = 0;
		ring[at++] = 0;
	}
	ring[at++] = kMiBatchBufferStart | kMiBatchBufferPerProcess;
	ring[at++] = (uint32)batchAddress;
	ring[at++] = (uint32)(batchAddress >> 32);

	if (fEngine->usesPipeControl) {
		// Empty the render caches first, then write the fence in a second
		// pipe control: the hardware dislikes being asked to do both at once.
		ring[at++] = kPipeControl(6);
		ring[at++] = kPipeControlStall | kPipeControlTlbInvalidate
			| kPipeControlRenderTargetFlush | kPipeControlDepthFlush
			| kPipeControlDataCacheFlush;
		ring[at++] = 0;
		ring[at++] = 0;
		ring[at++] = 0;
		ring[at++] = 0;

		ring[at++] = kPipeControl(6);
		ring[at++] = kPipeControlQwordWrite | kPipeControlGlobalGtt
			| kPipeControlFlush | kPipeControlStall;
		ring[at++] = fenceAddress;
		ring[at++] = 0;
		ring[at++] = seqno;
		ring[at++] = 0;
	} else {
		ring[at++] = kMiFlushDword | kMiFlushStoreDword;
		ring[at++] = fenceAddress | kMiFlushUseGlobalGtt;
		ring[at++] = 0;
		ring[at++] = seqno;
	}

	while (at < kCommandDwords)
		ring[at++] = kMiNoop;

	fRingTail = (fRingTail + kCommandDwords * 4) % fRingSize;
	FlushCpuCache(fRing.Address(), fRing.Size());
	_UpdateTail(fRingTail);

	// The descriptor names the context image and how it is addressed; the
	// port takes it as two writes, high half first.
	uint64 descriptor = fContext.GraphicsAddress()
		| (kContextLegacy64Bit << kContextAddressingShift)
		| kContextValid | kContextPrivilege | kContextForceRestore
		| ((fContext.GraphicsAddress() >> 12) << kContextIdShift);


	// Each logical context owns its status buffer. All submissions share the
	// physical engine lock, including the complete context-save interval.
	memset(fStatusPage.Address(), 0, fStatusPage.Size());
	FlushCpuCache(fStatusPage.Address(), fStatusPage.Size());
	_Write(fEngine->base + kRingHardwareStatusPage,
		(uint32)fStatusPage.GraphicsAddress());
	(void)_Read(fEngine->base + kRingHardwareStatusPage);

	// An empty second port, then ours: the hardware reads both.
	_Write(fEngine->base + kRingExeclistSubmitPort, 0);
	_Write(fEngine->base + kRingExeclistSubmitPort, 0);
	_Write(fEngine->base + kRingExeclistSubmitPort, (uint32)(descriptor >> 32));
	_Write(fEngine->base + kRingExeclistSubmitPort, (uint32)descriptor);

	status = _WaitSeqno(seqno, kIdleTimeout);
	if (status == B_OK)
		status = _WaitContextSaved(kIdleTimeout);
	if (status == B_OK) {
		// The engine has saved its image, so the timestamp inside it is the
		// one it stopped at. The difference since the last submission is the
		// time the hardware spent running this context and nothing else.
		FlushCpuCache(fContext.Address(), fContext.Size());
		uint32 timestamp = fRegisterState[kStateTimestamp];
		uint32 elapsed = timestamp - fLastTimestamp;
		fLastTimestamp = timestamp;
		fGpuTicks += elapsed;
		fScheduler->fTotalGpuTicks += elapsed;
	}
	_Forcewake(false);
	if (status != B_OK) {
		fFaulted = true;
		fScheduler->fFaulted = true;
		return status;
	}

	_fence = seqno;
	return B_OK;
}


// Not every register answers in the domain belonging to the engine reading it.
// The fuses and the frequency request both live in the domain Linux calls GT,
// which is the one the blitter uses, so an engine that is not the blitter has
// to take it as well. Only used while the device is being brought up, where
// nothing else is holding a domain.
status_t
RenderEngine::_ForcewakeGt(bool take)
{
	if (fEngine->forcewake == kForcewakeBlitter)
		return B_OK;

	_Write(kForcewakeBlitter, Masked(kForcewakeKernel, take));
	bigtime_t deadline = system_time() + kForcewakeTimeout;
	while (system_time() < deadline) {
		bool awake = (_Read(kForcewakeBlitterAck) & kForcewakeKernel) != 0;
		if (awake == take)
			return B_OK;
		spin(10);
	}
	return B_TIMED_OUT;
}


// Ask for the fastest frequency the part reports it can reach. There is no
// governor here and no power management interrupts to drive one, so this does
// not track load; what it does is stop the GPU asking for less than its own
// minimum, which is the state firmware leaves it in. Idle power is unaffected,
// because the GPU still powers itself down between submissions and a frequency
// only applies while it is running.
void
RenderEngine::_RequestMaximumFrequency()
{
	if (_ForcewakeGt(true) != B_OK) {
		_ForcewakeGt(false);
		return;
	}
	uint32 cap = _Read(kFrequencyCap);
	uint32 maximum = (cap & 0xff) * kFrequencyScaler;
	if (maximum != 0)
		_Write(kFrequencyRequest, maximum << kFrequencyRequestShift);
	_ForcewakeGt(false);
}


// Must be called with this engine's forcewake domain held: the fuse registers
// live in it and read back as zero without it. The layout is the generation 9
// one Linux decodes in gen9_sseu_info_init: three slices of four subslices of
// eight execution units, a slice enable field, a subslice disable field, and
// one execution unit disable register per slice.
void
RenderEngine::_ReadTopology()
{
	if (_ForcewakeGt(true) != B_OK) {
		_ForcewakeGt(false);
		return;
	}
	fTopology = Request<Topology>();
	fTopology.maxSlices = 3;
	fTopology.maxSubslices = 4;
	fTopology.maxEusPerSubslice = 8;

	uint32 fuse = _Read(kFuse2);
	fTopology.sliceMask = (fuse >> kFuse2SliceEnableShift) & 0x7;
	uint32 subslices = (~(fuse >> kFuse2SubsliceDisableShift)) & 0xf;

	for (uint32 slice = 0; slice < 3; slice++) {
		if ((fTopology.sliceMask & (1 << slice)) == 0)
			continue;
		fTopology.subsliceMask[slice] = subslices;
		uint32 disable = _Read(kEuDisable(slice));
		uint32 mask = 0;
		for (uint32 subslice = 0; subslice < 4; subslice++) {
			if ((subslices & (1 << subslice)) == 0)
				continue;
			mask |= (~(disable >> (subslice * 8)) & 0xff) << (subslice * 8);
		}
		fTopology.euMask[slice] = mask;
	}
	_ForcewakeGt(false);
}


status_t
RenderEngine::Status(EngineStatus& status)
{
	if (!fReady)
		return B_NO_INIT;

	MutexLocker locker(&fScheduler->fLock);
	status_t forcewake = _Forcewake(true);
	if (forcewake != B_OK)
		return forcewake;

	status.ringHead = _Read(fEngine->base + kRingHead);
	status.ringTail = _Read(fEngine->base + kRingTail);
	status.ringStart = _Read(fEngine->base + kRingStart);
	status.ringControl = _Read(fEngine->base + kRingControl);
	status.activeHead = _Read(fEngine->base + kRingActiveHead);
	status.instructionHeader = _Read(fEngine->base + kRingInstructionHeader);
	status.errorIdentity = _Read(fEngine->base + kRingErrorIdentity);
	status.miMode = _Read(fEngine->base + kRingMiMode);
	status.mode = _Read(fEngine->base + kRingMode);
	status.execlistStatusLow = _Read(fEngine->base + kRingExeclistStatus);
	status.execlistStatusHigh = _Read(fEngine->base + kRingExeclistStatus + 4);
	status.statusPointer = _Read(fEngine->base + kRingContextStatusPointer);
	status.interruptStatus = _Read(kGtInterruptStatus0);
	status.hardwareStatusAddress = _Read(fEngine->base + kRingHardwareStatusPage);

	_Forcewake(false);

	// Read the image back from memory rather than from any copy of it this
	// processor may still be holding.
	FlushCpuCache(fRegisterState, B_PAGE_SIZE);
	status.contextRingHead = fRegisterState[kStateRingHead];
	status.contextRingTail = fRegisterState[kStateRingTail];
	status.contextRingStart = fRegisterState[kStateRingStart];
	status.contextRingControl = fRegisterState[kStateRingControl];
	status.contextControl = fRegisterState[kStateContextControl];
	status.contextChanged = 0;
	status.contextFirstChange = 0;
	for (uint32 i = 0; i < B_PAGE_SIZE / sizeof(uint32); i++) {
		if (fRegisterState[i] == fStateSnapshot[i])
			continue;
		if (status.contextChanged == 0)
			status.contextFirstChange = i;
		status.contextChanged++;
	}
	status.ringFirstDword = *(const uint32*)fRing.Address();
	status.fence = _Seqno();
	status.submitted = fNextSeqno - 1;

	// The engine appends what it did with a context to its status page.
	const uint32* buffer = (const uint32*)fStatusPage.Address();
	for (uint32 i = 0; i < B_COUNT_OF(status.statusBuffer); i++)
		status.statusBuffer[i] = buffer[kStatusBufferIndex + i];

	return B_OK;
}


status_t
RenderEngine::Wait(uint64 fence, bigtime_t timeout)
{
	if (!fReady)
		return B_NO_INIT;
	if (fence == 0 || fence >= fNextSeqno || timeout < 0
		|| timeout > 10000000)
		return B_BAD_VALUE;
	return _WaitSeqno((uint32)fence, timeout);
}

}
