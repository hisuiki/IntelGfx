/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_BUFFER_OBJECT_H
#define INTEL_GFX_BUFFER_OBJECT_H

#include "GlobalGTT.h"

#include <KernelExport.h>

namespace IntelGfx {

class BufferObject {
public:
	BufferObject();
	~BufferObject();
	status_t Init(size_t size);
	area_id Area() const { return fArea; }
	size_t Size() const { return fSize; }
	// Kernel mapping of the buffer, for the driver's own objects.
	void* Address() const { return fAddress; }

	// Pins the buffer's pages into the global GTT, or releases them again.
	// A bound buffer stays bound until it is unbound or destroyed; the GTT
	// entries always go away before the pages they point at.
	status_t Bind(GlobalGTT& gtt);
	status_t Unbind();
	bool IsBound() const { return fGTT != NULL; }
	uint64 GraphicsAddress() const { return fGraphicsAddress; }

private:
	BufferObject(const BufferObject&) = delete;
	BufferObject& operator=(const BufferObject&) = delete;
	area_id fArea;
	size_t fSize;
	void* fAddress;
	GlobalGTT* fGTT;
	uint64 fGraphicsAddress;
};

}
#endif
