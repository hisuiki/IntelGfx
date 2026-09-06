/* SPDX-License-Identifier: MIT */
#include "RenderEngine.h"

#include "GpuHardware.h"

#include <string.h>
#include <util/AutoLock.h>

namespace IntelGfx {

// The GPU reads the context image and the ring out of ordinary cached memory.
// Haiku's kernel exports no way to write those lines back, so it is done here
// directly; a store fence alone would only order them, not land them.
static void
FlushRange(const void* address, size_t size)
{
	const uint8* line = (const uint8*)((addr_t)address & ~(addr_t)63);
	const uint8* end = (const uint8*)address + size;
	for (; line < end; line += 64)
		asm volatile("clflush %0" : : "m" (*line) : "memory");
	asm volatile("sfence" ::: "memory");
}


static const uint32 kRingSize = 16 * 1024;
static const uint32 kContextSize = 4 * B_PAGE_SIZE;
static const uint32 kStateOffset = B_PAGE_SIZE;	// the register image follows
												// the context's status page
static const uint32 kFenceOffset = 0x100;		// eight byte aligned, bit 5 clear
static const uint64 kContextId = 0x20;
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

#undef NOP
#undef LRI
#undef REG
#undef REG16
#undef POSTED


static void
WriteStateLayout(uint32* state, uint32 base)
{
	const uint8* data = kEngineStateLayout;
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
	fBase(kBlitterEngineBase),
	fReady(false),
	fRegisterState(NULL),
	fRingSize(kRingSize),
	fRingTail(0),
	fNextSeqno(1)
{
	mutex_init(&fLock, "intel_gfx engine");
}


RenderEngine::~RenderEngine()
{
	if (fReady) {
		// Leave the engine as it was found: no execution list, and nothing
		// pointing at memory that is about to be freed.
		if (_Forcewake(true) == B_OK) {
			_Write(fBase + kRingMode, Masked(kExeclistEnable, false));
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
	// The blitter's registers live in the domain generation 9 calls GT.
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


void
RenderEngine::_InitContext()
{
	uint8* context = (uint8*)fContext.Address();
	memset(context, 0, fContext.Size());

	fRegisterState = (uint32*)(context + kStateOffset);
	WriteStateLayout(fRegisterState, fBase);

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
		= (uint32)((uint64)fPageTables.Root() >> 32);
	fRegisterState[kStatePageDirectory0Lower]
		= (uint32)fPageTables.Root();
	// The ring must not come back stopped.
	fRegisterState[kStateMiMode + 1] = Masked(kStopRing, false);
}


status_t
RenderEngine::Init(addr_t registers, GlobalGTT& gtt)
{
	if (fReady)
		return B_BUSY;
	if (registers == 0 || !gtt.IsValid())
		return B_NOT_SUPPORTED;

	fRegisters = registers;

	struct { BufferObject* buffer; size_t size; } objects[] = {
		{ &fStatusPage, B_PAGE_SIZE },
		{ &fFencePage, B_PAGE_SIZE },
		{ &fContext, kContextSize },
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

	status_t status = fPageTables.Init();
	if (status != B_OK)
		return status;

	_InitContext();

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
	_Write(fBase + kRingHardwareStatusMask, ~0u);

	_Write(fBase + kRingMode, Masked(kExeclistEnable, true));
	_Write(fBase + kRingMiMode, Masked(kStopRing, false));
	_Write(fBase + kRingHardwareStatusPage,
		(uint32)fStatusPage.GraphicsAddress());
	(void)_Read(fBase + kRingHardwareStatusPage);

	_Forcewake(false);

	fReady = true;
	return B_OK;
}


status_t
RenderEngine::MapBuffer(area_id area, uint64 address)
{
	if (!fReady)
		return B_NO_INIT;
	return fPageTables.Map(area, address);
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
		status = fPageTables.MapPhysical(page, physical, B_PAGE_SIZE);
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
	return fPageTables.Unmap(address, size);
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
		if ((int32)(_Seqno() - seqno) >= 0)
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
	FlushRange(fRegisterState, B_PAGE_SIZE);
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

	MutexLocker locker(&fLock);

	// One submission at a time: the ring is only refilled once the engine has
	// finished with what was in it.
	if (fNextSeqno > 1) {
		status_t status = _WaitSeqno(fNextSeqno - 1, kIdleTimeout);
		if (status != B_OK)
			return status;
	}

	const uint32 kCommandDwords = 8;
	if (fRingTail + kCommandDwords * 4 > fRingSize) {
		// Pad the rest of the ring so the engine runs into the wrap cleanly.
		uint32* pad = (uint32*)((uint8*)fRing.Address() + fRingTail);
		for (uint32 i = 0; i < (fRingSize - fRingTail) / 4; i++)
			pad[i] = kMiNoop;
		fRingTail = 0;
	}

	uint32 seqno = fNextSeqno++;
	uint32* ring = (uint32*)((uint8*)fRing.Address() + fRingTail);
	uint32 fenceAddress = (uint32)fFencePage.GraphicsAddress() + kFenceOffset;

	// Run the client's commands, then flush and write the sequence number:
	// the flush is what makes everything the batch wrote visible before the
	// fence says it is.
	ring[0] = kMiBatchBufferStart | kMiBatchBufferPerProcess;
	ring[1] = (uint32)batchAddress;
	ring[2] = (uint32)(batchAddress >> 32);
	ring[3] = kMiFlushDword | kMiFlushStoreDword;
	ring[4] = fenceAddress | kMiFlushUseGlobalGtt;
	ring[5] = 0;
	ring[6] = seqno;
	ring[7] = kMiNoop;

	fRingTail += kCommandDwords * 4;
	FlushRange(ring, kCommandDwords * 4);
	_UpdateTail(fRingTail);

	// The descriptor names the context image and how it is addressed; the
	// port takes it as two writes, high half first.
	uint64 descriptor = fContext.GraphicsAddress()
		| (kContextLegacy64Bit << kContextAddressingShift)
		| kContextValid | kContextPrivilege | kContextForceRestore
		| (kContextId << kContextIdShift);

	status_t status = _Forcewake(true);
	if (status != B_OK)
		return status;

	// An empty second port, then ours: the hardware reads both.
	_Write(fBase + kRingExeclistSubmitPort, 0);
	_Write(fBase + kRingExeclistSubmitPort, 0);
	_Write(fBase + kRingExeclistSubmitPort, (uint32)(descriptor >> 32));
	_Write(fBase + kRingExeclistSubmitPort, (uint32)descriptor);

	_Forcewake(false);

	_fence = seqno;
	return B_OK;
}


status_t
RenderEngine::Status(EngineStatus& status)
{
	if (!fReady)
		return B_NO_INIT;

	MutexLocker locker(&fLock);
	status_t forcewake = _Forcewake(true);
	if (forcewake != B_OK)
		return forcewake;

	status.ringHead = _Read(fBase + kRingHead);
	status.ringTail = _Read(fBase + kRingTail);
	status.ringStart = _Read(fBase + kRingStart);
	status.ringControl = _Read(fBase + kRingControl);
	status.activeHead = _Read(fBase + kRingActiveHead);
	status.instructionHeader = _Read(fBase + kRingInstructionHeader);
	status.errorIdentity = _Read(fBase + kRingErrorIdentity);
	status.miMode = _Read(fBase + kRingMiMode);
	status.mode = _Read(fBase + kRingMode);
	status.execlistStatusLow = _Read(fBase + kRingExeclistStatus);
	status.execlistStatusHigh = _Read(fBase + kRingExeclistStatus + 4);
	status.statusPointer = _Read(fBase + kRingContextStatusPointer);
	status.interruptStatus = _Read(kGtInterruptStatus0);
	status.hardwareStatusAddress = _Read(fBase + kRingHardwareStatusPage);

	_Forcewake(false);

	// Read the image back from memory rather than from any copy of it this
	// processor may still be holding.
	FlushRange(fRegisterState, B_PAGE_SIZE);
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
	if (fence == 0 || fence > 0xffffffffULL)
		return B_BAD_VALUE;
	return _WaitSeqno((uint32)fence, timeout);
}

}
