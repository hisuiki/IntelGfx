/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_CACHE_H
#define INTEL_GFX_CACHE_H

#include <KernelExport.h>

namespace IntelGfx {
inline void FlushCpuCache(const void* address, size_t size)
{
	if (size == 0)
		return;
	const uint8* line = (const uint8*)((addr_t)address & ~(addr_t)63);
	const uint8* end = (const uint8*)address + size;
	for (; line < end; line += 64)
		asm volatile("clflush %0" : : "m" (*line) : "memory");
	asm volatile("mfence" ::: "memory");
}
}
#endif
