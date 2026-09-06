/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_PAGE_TABLES_H
#define INTEL_GFX_PAGE_TABLES_H

#include <KernelExport.h>
#include <lock.h>

namespace IntelGfx {

// The four level per-process page tables an execlist context runs against.
// A context always needs valid tables even when the work it runs addresses
// memory through the global GTT, because the hardware walks them on restore.
//
// The tables are ordinary locked kernel pages; the GPU reaches them by
// physical address, so nothing here has to be visible through the aperture.
// Unmapped addresses resolve to a scratch page rather than faulting, which
// keeps a mistake in a command buffer from taking the engine down.
class PageTables {
public:
	PageTables();
	~PageTables();

	status_t Init();
	bool IsValid() const { return fRoot != 0; }

	// Physical address of the top level table, which is what the context
	// image hands to the hardware.
	phys_addr_t Root() const { return fRoot; }

	// Maps the pages of an area at the given GPU address. The address and the
	// area's size are page aligned; overlapping maps are rejected.
	status_t Map(area_id area, uint64 address);
	// Maps memory this object does not own, such as the display's
	// framebuffer, which the global page table already points at.
	status_t MapPhysical(uint64 address, phys_addr_t physical, uint64 size);
	status_t Unmap(uint64 address, uint64 size);

private:
	PageTables(const PageTables&) = delete;
	PageTables& operator=(const PageTables&) = delete;

	struct Table {
		area_id area;
		uint64* entries;
		phys_addr_t physical;
	};

	status_t _CreateTable(Table& table);
	void _DeleteTable(Table& table);
	// Returns the table one level down, creating it when asked to.
	status_t _Descend(Table& table, uint32 index, bool create, Table& _child);
	status_t _Walk(uint64 address, bool create, Table& _pageTable,
		uint32& _index);

	Table fTables[512];			// every table this object created
	uint32 fTableCount;
	Table fTop;
	phys_addr_t fRoot;
	area_id fScratchArea;
	phys_addr_t fScratchPage;
	mutex fLock;
};

}
#endif
