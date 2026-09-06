/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_RENDER_ENGINE_H
#define INTEL_GFX_RENDER_ENGINE_H

#include "BufferObject.h"
#include "GlobalGTT.h"
#include "IntelGfxABI.h"
#include "PageTables.h"

#include <KernelExport.h>
#include <lock.h>

namespace IntelGfx {

// The blitter engine of a generation 9 GPU, driven the way the hardware
// expects since generation 8: work is described by a logical ring context
// whose address is handed to the engine's execution list submit port, and the
// engine loads the context, runs the ring, and saves the context back.
//
// The blitter is used rather than the render engine because its context is
// the simplest one that still runs ordinary memory commands, and because
// nothing else in the system is using it while the display driver works.
//
// Completion is polled: a submission ends with a flush that writes its
// sequence number to a status page, and a fence is that number. No interrupt
// is enabled, and the engine's interrupts are masked so that work here cannot
// disturb the display driver's interrupt handling.
class RenderEngine {
public:
	RenderEngine();
	~RenderEngine();

	status_t Init(addr_t registers, GlobalGTT& gtt);
	bool IsReady() const { return fReady; }

	// Client buffers are mapped into the engine's page tables at the same
	// address their global mapping uses, so one address means the same thing
	// to commands whichever address space they run against.
	status_t MapBuffer(area_id area, uint64 address);
	status_t UnmapBuffer(uint64 address, uint64 size);

	status_t Submit(uint64 batchAddress, uint32 batchLength, uint64& _fence);
	status_t Wait(uint64 fence, bigtime_t timeout);
	status_t Status(EngineStatus& status);
	uint64 CompletedFence();

private:
	RenderEngine(const RenderEngine&) = delete;
	RenderEngine& operator=(const RenderEngine&) = delete;

	uint32 _Read(uint32 offset) const;
	void _Write(uint32 offset, uint32 value);
	status_t _Forcewake(bool take);
	void _InitContext();
	void _UpdateTail(uint32 tail);
	uint32 _Seqno();
	status_t _WaitSeqno(uint32 seqno, bigtime_t timeout);

	addr_t fRegisters;
	uint32 fBase;
	bool fReady;

	BufferObject fStatusPage;	// the engine's own status page
	BufferObject fFencePage;	// where a submission writes its sequence number
	BufferObject fContext;		// logical ring context
	BufferObject fRing;
	PageTables fPageTables;

	uint32* fRegisterState;		// the context's register image
	uint32 fStateSnapshot[B_PAGE_SIZE / sizeof(uint32)];
	uint32 fRingSize;
	uint32 fRingTail;
	uint32 fNextSeqno;
	mutex fLock;
};

}
#endif
