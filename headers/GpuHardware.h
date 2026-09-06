/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_GPU_HARDWARE_H
#define INTEL_GFX_GPU_HARDWARE_H

#include <SupportDefs.h>

// Register offsets and command opcodes for generation 8 and 9 engines, as
// described by the Broadwell, Skylake and Kaby Lake programmer's reference
// manuals. Everything here is hardware layout, so it stays free of policy.
namespace IntelGfx {

// Engine register windows. Each engine's registers are at a fixed offset from
// its base, so one set of definitions covers all of them.
static const uint32 kRenderEngineBase = 0x02000;
static const uint32 kBlitterEngineBase = 0x22000;

static const uint32 kRingTail = 0x30;
static const uint32 kRingHead = 0x34;
static const uint32 kRingStart = 0x38;
static const uint32 kRingControl = 0x3c;
static const uint32 kRingHardwareStatusPage = 0x80;
static const uint32 kRingHardwareStatusMask = 0x98;
static const uint32 kRingActiveHead = 0x74;
static const uint32 kRingInstructionHeader = 0x68;
static const uint32 kRingErrorIdentity = 0xb0;
static const uint32 kRingMiMode = 0x9c;
static const uint32 kRingBatchBufferState = 0x110;
static const uint32 kRingSecondBatchBufferAddress = 0x114;
static const uint32 kRingSecondBatchBufferState = 0x118;
static const uint32 kRingSecondBatchBufferAddressUpper = 0x11c;
static const uint32 kRingBatchBufferAddress = 0x140;
static const uint32 kRingBatchBufferAddressUpper = 0x168;
static const uint32 kRingBatchPerContextPointer = 0x1c0;
static const uint32 kRingIndirectContext = 0x1c4;
static const uint32 kRingIndirectContextOffset = 0x1c8;
static const uint32 kRingExeclistSubmitPort = 0x230;
static const uint32 kRingExeclistStatus = 0x234;
static const uint32 kRingContextControl = 0x244;
static const uint32 kRingPageDirectory = 0x270;	// four pairs, low then high
static const uint32 kRingMode = 0x29c;
static const uint32 kRingContextStatusPointer = 0x3a0;
static const uint32 kRingContextTimestamp = 0x3a8;

// Bits of the registers above.
static const uint32 kRingValid = 1 << 0;
static const uint32 kStopRing = 1 << 8;
static const uint32 kExeclistEnable = 1 << 15;		// GFX_RUN_MODE
static const uint32 kBatchBufferPerProcessGtt = 1 << 5;
static const uint32 kContextControlInhibitSyn = 1 << 3;
static const uint32 kContextControlRestoreInhibit = 1 << 0;

// Many mode registers only accept a write when the matching bit of the upper
// half says so, which also makes such a write safe beside another writer.
static constexpr uint32 Masked(uint32 bits, bool set)
{
	return (bits << 16) | (set ? bits : 0);
}

// Forcewake. The hardware may have powered an engine down; taking the domain
// keeps it awake and its registers readable for as long as it is held.
static const uint32 kForcewakeRender = 0xa278;
static const uint32 kForcewakeRenderAck = 0x0d84;
static const uint32 kForcewakeBlitter = 0xa188;
static const uint32 kForcewakeBlitterAck = 0x130044;
static const uint32 kForcewakeMedia = 0xa270;
static const uint32 kForcewakeMediaAck = 0x0d88;
static const uint32 kForcewakeKernel = 1 << 0;

// Interrupts are masked rather than handled: this driver polls.
static const uint32 kGtInterruptStatus0 = 0x44300;	// raw, whatever is masked
static const uint32 kGtInterruptMask0 = 0x44304;	// render and blitter
static const uint32 kGtInterruptEnable0 = 0x4430c;

// The engine reports what it did with a context by appending to the status
// buffer inside its status page, six pairs of dwords starting here.
static const uint32 kStatusBufferIndex = 0x10;

// Execlist context descriptor, written to the submit port as two dwords.
static const uint64 kContextValid = 1ULL << 0;
static const uint64 kContextPrivilege = 1ULL << 8;
static const uint32 kContextAddressingShift = 3;
static const uint64 kContextLegacy64Bit = 3;		// four level page tables
static const uint64 kContextForceRestore = 1ULL << 2;
static const uint32 kContextIdShift = 32;

// Command opcodes. The length field of a command counts its dwords minus two.
static constexpr uint32 MiInstruction(uint32 opcode)
{
	return opcode << 23;
}

static constexpr uint32 kMiNoop = 0;
static constexpr uint32 kMiBatchBufferEnd = MiInstruction(0x0a);
static constexpr uint32 kMiStoreDataImmediate = MiInstruction(0x20) | 2;
static const uint32 kMiStoreGlobalGtt = 1 << 22;
static constexpr uint32 kMiBatchBufferStart = MiInstruction(0x31) | 1;
static const uint32 kMiBatchBufferPerProcess = 1 << 8;
static constexpr uint32 kMiLoadRegisterImmediate = MiInstruction(0x22);
static const uint32 kMiLoadRegisterPosted = 1 << 12;
// A flush that also writes a value: the write lands after everything the
// commands before it did, which is what makes it usable as a fence.
static constexpr uint32 kMiFlushDword = MiInstruction(0x26) | 2;
static const uint32 kMiFlushStoreDword = 1 << 14;
static const uint32 kMiFlushUseGlobalGtt = 1 << 2;	// set in the address

// The command values these produce are what the hardware documents them as;
// a build where they came out as anything else would be silently wrong.
static_assert(kMiBatchBufferStart == 0x18800001, "MI_BATCH_BUFFER_START");
static_assert(kMiStoreDataImmediate == 0x10000002, "MI_STORE_DWORD_IMM");
static_assert(kMiBatchBufferEnd == 0x05000000, "MI_BATCH_BUFFER_END");
static_assert(kMiFlushDword == 0x13000002, "MI_FLUSH_DW");
static_assert(kMiLoadRegisterImmediate == 0x11000000, "MI_LOAD_REGISTER_IMM");

// Blitter commands. The colour fill takes seven dwords from generation 8,
// where the destination address grew to two of them, and the copy ten.
static constexpr uint32 kBlitInstruction(uint32 opcode)
{
	return (2 << 29) | (opcode << 22);
}

static constexpr uint32 kXyColorBlit = kBlitInstruction(0x50) | (7 - 2);
static constexpr uint32 kXySourceCopyBlit = kBlitInstruction(0x53) | (10 - 2);
static constexpr uint32 kBlitWriteRgb = 1 << 20;
static constexpr uint32 kBlitWriteAlpha = 2 << 20;
static constexpr uint32 kBlitDepth32 = 3 << 24;
static constexpr uint32 kBlitRopColorCopy = 0xf0 << 16;
static constexpr uint32 kBlitRopSourceCopy = 0xcc << 16;

// The render engine does not take the flush the other engines use. Its
// caches are emptied with a pipe control, which is also how it writes a value
// out once everything before it has landed.
static constexpr uint32 kPipeControl(uint32 length)
{
	return (3 << 29) | (3 << 27) | (2 << 24) | (length - 2);
}

static constexpr uint32 kPipeControlGlobalGtt = 1 << 24;
static constexpr uint32 kPipeControlTlbInvalidate = 1 << 18;
static constexpr uint32 kPipeControlStall = 1 << 20;
static constexpr uint32 kPipeControlQwordWrite = 1 << 14;
static constexpr uint32 kPipeControlRenderTargetFlush = 1 << 12;
static constexpr uint32 kPipeControlFlush = 1 << 7;
static constexpr uint32 kPipeControlDataCacheFlush = 1 << 5;
static constexpr uint32 kPipeControlDepthFlush = 1 << 0;

// The page attribute table, which is what the bits in a page table entry
// actually select. Every access this driver makes lands on entry zero: the
// per-process entries are written without cacheability bits, and the global
// table has no such bits at all, its accesses always using entry zero. So
// entry zero decides whether the processor and the GPU see each other's
// writes, and nothing on a Haiku system programs this table.
static const uint32 kPrivatePatLow = 0x40e0;
static const uint32 kPrivatePatHigh = 0x40e4;

static constexpr uint64 kPatEntry(uint32 index, uint64 value)
{
	return value << (index * 8);
}

static constexpr uint64 kPatUncached = 0 << 0;
static constexpr uint64 kPatWriteCombining = 1 << 0;
static constexpr uint64 kPatWriteThrough = 2 << 0;
static constexpr uint64 kPatWriteBack = 3 << 0;
static constexpr uint64 kPatLastLevelCache = 1 << 2;
static constexpr uint64 kPatBothCaches = 2 << 2;
static constexpr uint64 kPatAge(uint64 age) { return age << 4; }

// Page table entries of the per-process page tables. Cacheability is chosen
// by an index into the PAT registers, formed from three scattered bits; index
// zero is the write-back entry the firmware and every other driver assume.
static const uint64 kPageEntryPresent = 1ULL << 0;
static const uint64 kPageEntryWritable = 1ULL << 1;
static const uint64 kPageEntryAddressMask = 0x0000007ffffff000ULL;

}
#endif
