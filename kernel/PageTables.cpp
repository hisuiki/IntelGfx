/* SPDX-License-Identifier: MIT */
#include "PageTables.h"

#include "GpuHardware.h"

#include <string.h>
#include <util/AutoLock.h>

namespace IntelGfx {

static const uint32 kEntriesPerTable = 512;
static const uint64 kPageMask = (uint64)B_PAGE_SIZE - 1;

// The four levels a 48 bit address is walked through, top first.
static inline uint32
IndexAt(uint64 address, uint32 level)
{
	static const uint32 kShift[4] = { 39, 30, 21, 12 };
	return (uint32)((address >> kShift[level]) & (kEntriesPerTable - 1));
}


PageTables::PageTables()
	:
	fTableCount(0),
	fRoot(0),
	fScratchArea(-1),
	fScratchPage(0)
{
	memset(fTables, 0, sizeof(fTables));
	memset(&fTop, 0, sizeof(fTop));
	fTop.area = -1;
	mutex_init(&fLock, "intel_gfx page tables");
}


PageTables::~PageTables()
{
	for (uint32 i = 0; i < fTableCount; i++)
		_DeleteTable(fTables[i]);
	if (fScratchArea >= 0)
		delete_area(fScratchArea);
	mutex_destroy(&fLock);
}


status_t
PageTables::_CreateTable(Table& table)
{
	if (fTableCount == B_COUNT_OF(fTables))
		return B_NO_MEMORY;

	void* address = NULL;
	table.area = create_area("intel_gfx page table", &address,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (table.area < 0)
		return table.area;

	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK || ((uint64)entry.address & ~kPageEntryAddressMask) != 0) {
		delete_area(table.area);
		table.area = -1;
		return status != B_OK ? status : B_BAD_ADDRESS;
	}

	memset(address, 0, B_PAGE_SIZE);
	table.entries = (uint64*)address;
	table.physical = entry.address;
	fTables[fTableCount++] = table;
	return B_OK;
}


void
PageTables::_DeleteTable(Table& table)
{
	if (table.area >= 0)
		delete_area(table.area);
	table.area = -1;
	table.entries = NULL;
}


status_t
PageTables::Init()
{
	if (IsValid())
		return B_BUSY;

	void* scratch = NULL;
	fScratchArea = create_area("intel_gfx gpu scratch", &scratch,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fScratchArea < 0)
		return fScratchArea;
	memset(scratch, 0, B_PAGE_SIZE);

	physical_entry entry;
	status_t status = get_memory_map(scratch, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	fScratchPage = entry.address;

	// Every level gets a scratch table of its own, each one filled with the
	// level below it, so that an address nothing was mapped at still walks to
	// a real page instead of faulting the engine.
	Table scratchTables[3];
	uint64 below = (uint64)fScratchPage | kPageEntryPresent | kPageEntryWritable;
	for (int level = 2; level >= 0; level--) {
		status = _CreateTable(scratchTables[level]);
		if (status != B_OK)
			return status;
		for (uint32 i = 0; i < kEntriesPerTable; i++)
			scratchTables[level].entries[i] = below;
		below = (uint64)scratchTables[level].physical | kPageEntryPresent
			| kPageEntryWritable;
	}

	status = _CreateTable(fTop);
	if (status != B_OK)
		return status;
	for (uint32 i = 0; i < kEntriesPerTable; i++)
		fTop.entries[i] = below;

	fRoot = fTop.physical;
	return B_OK;
}


status_t
PageTables::_Descend(Table& table, uint32 index, bool create, Table& _child)
{
	uint64 entry = table.entries[index];
	phys_addr_t physical = (phys_addr_t)(entry & kPageEntryAddressMask);

	for (uint32 i = 0; i < fTableCount; i++) {
		if (fTables[i].physical == physical) {
			// A scratch table is shared by everything above it, so it may
			// only be descended into, never written through.
			_child = fTables[i];
			return B_OK;
		}
	}

	if (!create)
		return B_ENTRY_NOT_FOUND;
	return B_ERROR;
}


status_t
PageTables::_Walk(uint64 address, bool create, Table& _pageTable,
	uint32& _index)
{
	// The scratch tables are the first three this object made, deepest last.
	Table table = fTop;
	for (uint32 level = 0; level < 3; level++) {
		uint32 index = IndexAt(address, level);
		Table child;
		status_t status = _Descend(table, index, create, child);
		if (status != B_OK)
			return status;

		bool isScratch = false;
		for (uint32 i = 0; i < 3; i++) {
			if (fTables[i].physical == child.physical)
				isScratch = true;
		}
		if (isScratch) {
			if (!create)
				return B_ENTRY_NOT_FOUND;

			// Replace the shared scratch table with one of this address's
			// own, seeded with the same contents so its other entries keep
			// resolving where they did.
			Table replacement;
			status = _CreateTable(replacement);
			if (status != B_OK)
				return status;
			for (uint32 i = 0; i < kEntriesPerTable; i++)
				replacement.entries[i] = child.entries[i];
			table.entries[index] = (uint64)replacement.physical
				| kPageEntryPresent | kPageEntryWritable;
			child = replacement;
		}
		table = child;
	}

	_pageTable = table;
	_index = IndexAt(address, 3);
	return B_OK;
}


status_t
PageTables::Map(area_id area, uint64 address)
{
	if (!IsValid())
		return B_NO_INIT;
	if ((address & kPageMask) != 0)
		return B_BAD_VALUE;

	area_info info;
	status_t status = get_area_info(area, &info);
	if (status != B_OK)
		return status;
	uint64 size = ((uint64)info.size + kPageMask) & ~kPageMask;

	MutexLocker locker(&fLock);
	for (uint64 offset = 0; offset < size; offset += B_PAGE_SIZE) {
		physical_entry entry;
		status = get_memory_map((void*)((addr_t)info.address + offset),
			B_PAGE_SIZE, &entry, 1);
		if (status != B_OK)
			return status;
		if (((uint64)entry.address & ~kPageEntryAddressMask) != 0)
			return B_BAD_ADDRESS;

		Table pageTable;
		uint32 index;
		status = _Walk(address + offset, true, pageTable, index);
		if (status != B_OK)
			return status;

		pageTable.entries[index] = (uint64)entry.address | kPageEntryPresent
			| kPageEntryWritable;
	}

	memory_write_barrier();
	return B_OK;
}


status_t
PageTables::Unmap(uint64 address, uint64 size)
{
	if (!IsValid())
		return B_NO_INIT;
	if ((address & kPageMask) != 0)
		return B_BAD_VALUE;

	size = (size + kPageMask) & ~kPageMask;
	uint64 scratch = (uint64)fScratchPage | kPageEntryPresent
		| kPageEntryWritable;

	MutexLocker locker(&fLock);
	for (uint64 offset = 0; offset < size; offset += B_PAGE_SIZE) {
		Table pageTable;
		uint32 index;
		if (_Walk(address + offset, false, pageTable, index) != B_OK)
			continue;
		pageTable.entries[index] = scratch;
	}

	memory_write_barrier();
	return B_OK;
}

}
