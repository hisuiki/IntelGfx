/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_GLOBAL_GTT_H
#define INTEL_GFX_GLOBAL_GTT_H

#include <KernelExport.h>
#include <lock.h>

namespace IntelGfx {

// The device's global GTT, programmed directly rather than through Haiku's
// agp_gart bus manager: that manager still writes the 32 bit entries of the
// pre-Broadwell layout, at the offset the page table had before generation 8,
// so nothing it binds is reachable by an eighth or ninth generation GPU.
//
// One instance belongs to a device and is shared by all of its clients. It
// only manages the part of the address space above the aperture, which is
// what agp_gart and the display driver hand out between themselves, so the
// two allocators can never collide. Addresses are byte offsets into the
// global address space, the form the display engine and the command streamer
// consume; unbound pages in the managed range point at a scratch page, so a
// stray access reads zeroes instead of memory belonging to something else.
class GlobalGTT {
public:
	GlobalGTT();
	~GlobalGTT();

	// registers is the mapped GTTMMADR BAR and barSize its full size;
	// tableSize is the configured page table size read from PCI config, and
	// reserved the leading part of the address space left to agp_gart.
	status_t Init(addr_t registers, uint64 barSize, uint32 generation,
		uint64 tableSize, uint64 reserved);

	bool IsValid() const { return fEntries != 0; }
	uint64 Base() const { return fBase; }
	uint64 Size() const { return fSize; }

	// The physical page a global address currently resolves to, including
	// addresses below the managed range, such as the display's framebuffer.
	status_t Lookup(uint64 address, phys_addr_t& _physical) const;

	status_t Bind(area_id area, uint64& _address);
	status_t Unbind(uint64 address, uint64 size);

private:
	GlobalGTT(const GlobalGTT&) = delete;
	GlobalGTT& operator=(const GlobalGTT&) = delete;

	status_t _Allocate(uint64 count, uint64& _first);
	status_t _MapArea(uint64 first, addr_t address, uint64 count);
	void _Fill(uint64 first, uint64 count, uint64 entry);
	uint64 _ScratchEntry() const;
	void _WriteEntry(uint64 page, uint64 entry);
	void _Flush();

	bool _IsUsed(uint64 page) const
		{ return (fUsed[page / 8] & (1 << (page % 8))) != 0; }
	void _SetUsed(uint64 page, bool used);

	addr_t fRegisters;
	addr_t fEntries;
	uint64 fEntryCount;
	uint64 fFirstPage;
	uint64 fPageCount;
	uint64 fBase;
	uint64 fSize;
	area_id fScratchArea;
	phys_addr_t fScratchPage;
	uint8* fUsed;
	mutex fLock;
};

}
#endif
