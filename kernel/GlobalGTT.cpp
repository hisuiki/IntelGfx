/* SPDX-License-Identifier: MIT */
#include "GlobalGTT.h"

#include "GpuHardware.h"

#include <stdlib.h>
#include <string.h>
#include <util/AutoLock.h>

namespace IntelGfx {

// Page table entry bits from generation 8 on: a 64 bit entry holding the
// page's physical address and a present bit. Cacheability is not part of the
// entry any more; it comes from the MOCS state of whatever accesses the page.
static const uint64 kEntryPresent = 1ULL << 0;
static const uint64 kEntryAddressMask = 0x0000007ffffff000ULL;

// Writing this register makes the GPU drop its cached page table entries.
static const uint32 kFlushControl = 0x101008;
static const uint32 kFlushEnable = 1;

// The managed range only has to cover what clients may pin, and every page of
// it is written at least once during initialization, so it stays modest.
static const uint64 kManagedSize = 256ULL * 1024 * 1024;

static const uint64 kPageMask = (uint64)B_PAGE_SIZE - 1;


GlobalGTT::GlobalGTT()
	:
	fRegisters(0),
	fEntries(0),
	fEntryCount(0),
	fFirstPage(0),
	fPageCount(0),
	fBase(0),
	fSize(0),
	fScratchArea(-1),
	fScratchPage(0),
	fUsed(NULL)
{
	mutex_init(&fLock, "intel_gfx gtt");
}


GlobalGTT::~GlobalGTT()
{
	if (fEntries != 0) {
		// The scratch page goes away with this object, so the range is left
		// not present rather than pointing at memory that has been recycled.
		_Fill(0, fPageCount, 0);
		_Flush();
	}
	if (fScratchArea >= 0)
		delete_area(fScratchArea);
	free(fUsed);
	mutex_destroy(&fLock);
}


status_t
GlobalGTT::Init(addr_t registers, uint64 barSize, uint32 generation,
	uint64 tableSize, uint64 reserved)
{
	if (fEntries != 0)
		return B_BUSY;
	if (generation < 8) {
		// Earlier generations use 32 bit entries in the lower half of the
		// BAR. They are not the target of this driver, and guessing at their
		// layout would be worse than reporting no GPU address space at all.
		return B_NOT_SUPPORTED;
	}
	if (registers == 0 || tableSize == 0 || tableSize > barSize / 2)
		return B_BAD_VALUE;

	// The page table occupies the upper half of the register BAR.
	addr_t entries = registers + (addr_t)(barSize / 2);
	uint64 entryCount = tableSize / sizeof(uint64);
	uint64 addressSpace = entryCount * B_PAGE_SIZE;

	reserved = (reserved + kPageMask) & ~kPageMask;
	if (reserved >= addressSpace)
		return B_NO_MEMORY;
	uint64 size = addressSpace - reserved;
	if (size > kManagedSize)
		size = kManagedSize;
	uint64 pages = size / B_PAGE_SIZE;

	void* scratch = NULL;
	area_id scratchArea = create_area("intel_gfx gtt scratch", &scratch,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (scratchArea < 0)
		return scratchArea;
	memset(scratch, 0, B_PAGE_SIZE);

	physical_entry entry;
	status_t status = get_memory_map(scratch, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK) {
		delete_area(scratchArea);
		return status;
	}
	if (((uint64)entry.address & ~kEntryAddressMask) != 0) {
		delete_area(scratchArea);
		return B_BAD_ADDRESS;
	}

	uint8* used = (uint8*)malloc((size_t)((pages + 7) / 8));
	if (used == NULL) {
		delete_area(scratchArea);
		return B_NO_MEMORY;
	}
	memset(used, 0, (size_t)((pages + 7) / 8));

	fRegisters = registers;
	fEntries = entries;
	fEntryCount = entryCount;
	fFirstPage = reserved / B_PAGE_SIZE;
	fPageCount = pages;
	fBase = reserved;
	fSize = size;
	fScratchArea = scratchArea;
	fScratchPage = entry.address;
	fUsed = used;

	// Say what the cacheability bits of a page table entry mean before using
	// any. These are the assignments Linux makes on the same hardware, and
	// they matter because everything here lands on entry zero: written back
	// and kept in the last level cache, which is what lets the processor and
	// the GPU see each other's writes without either flushing by hand.
	uint64 attributes =
		kPatEntry(0, kPatWriteBack | kPatLastLevelCache)
		| kPatEntry(1, kPatWriteCombining | kPatBothCaches)
		| kPatEntry(2, kPatWriteBack)			// scanout with an eLLC
		| kPatEntry(3, kPatUncached)
		| kPatEntry(4, kPatWriteBack | kPatBothCaches | kPatAge(0))
		| kPatEntry(5, kPatWriteBack | kPatBothCaches | kPatAge(1))
		| kPatEntry(6, kPatWriteBack | kPatBothCaches | kPatAge(2))
		| kPatEntry(7, kPatWriteBack | kPatBothCaches | kPatAge(3));
	*(volatile uint32*)(fRegisters + kPrivatePatLow) = (uint32)attributes;
	*(volatile uint32*)(fRegisters + kPrivatePatHigh)
		= (uint32)(attributes >> 32);
	(void)*(volatile uint32*)(fRegisters + kPrivatePatLow);

	// Whatever the firmware left in this range must not alias memory that was
	// never bound here.
	_Fill(0, fPageCount, _ScratchEntry());
	_Flush();
	return B_OK;
}


status_t
GlobalGTT::Lookup(uint64 address, phys_addr_t& _physical) const
{
	if (!IsValid())
		return B_NOT_SUPPORTED;
	if ((address & kPageMask) != 0)
		return B_BAD_VALUE;

	uint64 page = address / B_PAGE_SIZE;
	if (page >= fEntryCount)
		return B_BAD_VALUE;

	uint64 entry = *(volatile uint64*)(fEntries
		+ (addr_t)(page * sizeof(uint64)));
	if ((entry & kEntryPresent) == 0)
		return B_ENTRY_NOT_FOUND;

	_physical = (phys_addr_t)(entry & kEntryAddressMask);
	return B_OK;
}


status_t
GlobalGTT::Bind(area_id area, uint64& _address)
{
	if (!IsValid())
		return B_NOT_SUPPORTED;

	area_info info;
	status_t status = get_area_info(area, &info);
	if (status != B_OK)
		return status;
	uint64 size = ((uint64)info.size + kPageMask) & ~kPageMask;
	if (size == 0 || size > fSize)
		return B_BAD_VALUE;

	MutexLocker locker(&fLock);
	uint64 first = 0;
	status = _Allocate(size / B_PAGE_SIZE, first);
	if (status != B_OK)
		return status;

	status = _MapArea(first, (addr_t)info.address, size / B_PAGE_SIZE);
	if (status != B_OK) {
		_Fill(first, size / B_PAGE_SIZE, _ScratchEntry());
		for (uint64 i = 0; i < size / B_PAGE_SIZE; i++)
			_SetUsed(first + i, false);
		_Flush();
		return status;
	}

	_Flush();
	_address = fBase + first * B_PAGE_SIZE;
	return B_OK;
}


status_t
GlobalGTT::Unbind(uint64 address, uint64 size)
{
	if (!IsValid())
		return B_NOT_SUPPORTED;

	size = (size + kPageMask) & ~kPageMask;
	if ((address & kPageMask) != 0 || address < fBase || size == 0
		|| address - fBase >= fSize || size > fSize - (address - fBase))
		return B_BAD_VALUE;

	uint64 first = (address - fBase) / B_PAGE_SIZE;
	uint64 count = size / B_PAGE_SIZE;

	MutexLocker locker(&fLock);
	for (uint64 i = 0; i < count; i++) {
		if (!_IsUsed(first + i))
			return B_BAD_VALUE;
	}

	_Fill(first, count, _ScratchEntry());
	_Flush();
	for (uint64 i = 0; i < count; i++)
		_SetUsed(first + i, false);
	return B_OK;
}


status_t
GlobalGTT::_Allocate(uint64 count, uint64& _first)
{
	uint64 run = 0;
	for (uint64 page = 0; page < fPageCount; page++) {
		if (_IsUsed(page)) {
			run = 0;
			continue;
		}
		if (++run < count)
			continue;

		uint64 first = page + 1 - count;
		for (uint64 i = 0; i < count; i++)
			_SetUsed(first + i, true);
		_first = first;
		return B_OK;
	}
	return B_NO_MEMORY;
}


status_t
GlobalGTT::_MapArea(uint64 first, addr_t address, uint64 count)
{
	// The pages of a locked area are rarely contiguous, so this walks the
	// memory map in chunks small enough that one lookup always describes the
	// whole chunk, even when every page of it stands on its own. The table
	// holds one entry more than that: get_memory_map() closes the list with
	// an empty entry and reports an overflow if there is no room for it.
	const uint32 kChunkPages = 32;
	physical_entry table[kChunkPages + 1];

	for (uint64 done = 0; done < count; ) {
		uint64 chunk = count - done;
		if (chunk > kChunkPages)
			chunk = kChunkPages;

		status_t status = get_memory_map((void*)(address + done * B_PAGE_SIZE),
			(size_t)(chunk * B_PAGE_SIZE), table, kChunkPages + 1);
		if (status != B_OK)
			return status;

		uint64 page = 0;
		for (uint32 i = 0; i < kChunkPages && page < chunk; i++) {
			for (phys_size_t offset = 0; offset < table[i].size
					&& page < chunk; offset += B_PAGE_SIZE, page++) {
				phys_addr_t physical = table[i].address + offset;
				if (((uint64)physical & ~kEntryAddressMask) != 0)
					return B_BAD_ADDRESS;
				_WriteEntry(fFirstPage + first + done + page,
					(uint64)physical | kEntryPresent);
			}
		}
		if (page != chunk)
			return B_BAD_VALUE;

		done += chunk;
	}
	return B_OK;
}


void
GlobalGTT::_Fill(uint64 first, uint64 count, uint64 entry)
{
	for (uint64 i = 0; i < count; i++)
		_WriteEntry(fFirstPage + first + i, entry);
}


uint64
GlobalGTT::_ScratchEntry() const
{
	return (uint64)fScratchPage | kEntryPresent;
}


void
GlobalGTT::_WriteEntry(uint64 page, uint64 entry)
{
	*(volatile uint64*)(fEntries + (addr_t)(page * sizeof(uint64))) = entry;
}


void
GlobalGTT::_Flush()
{
	// Read the last entry back first: the page table lives behind a PCI BAR,
	// so this is what forces the preceding writes out before the GPU is told
	// to drop the entries it had cached.
	(void)*(volatile uint64*)(fEntries
		+ (addr_t)((fFirstPage + fPageCount - 1) * sizeof(uint64)));
	*(volatile uint32*)(fRegisters + kFlushControl) = kFlushEnable;
}


void
GlobalGTT::_SetUsed(uint64 page, bool used)
{
	if (used)
		fUsed[page / 8] |= 1 << (page % 8);
	else
		fUsed[page / 8] &= ~(1 << (page % 8));
}

}
