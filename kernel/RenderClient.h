/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_RENDER_CLIENT_H
#define INTEL_GFX_RENDER_CLIENT_H

#include "IntelGfxABI.h"
#include "BufferObject.h"
#include "GlobalGTT.h"
#include "RenderEngine.h"
#include <lock.h>

struct intel_info;

namespace IntelGfx {

class RenderClient {
public:
	// The GTT belongs to the device and is shared by all of its clients; it
	// may be NULL on hardware this driver has no address space for.
	RenderClient(intel_info* device, const DeviceInfo& info, GlobalGTT* gtt,
		RenderEngine* engine);
	~RenderClient();
	intel_info* Device() const { return fDevice; }
	status_t Ioctl(uint32 operation, void* userBuffer, size_t length);
private:
	BufferObject* _Find(uint32 handle, uint32* _slot = NULL) const;

	intel_info* fDevice;
	DeviceInfo fInfo;
	GlobalGTT* fGTT;
	RenderEngine* fEngine;
	mutex fLock;
	BufferObject* fBuffers[kMaxBuffers];
	uint32 fHandles[kMaxBuffers];
	uint32 fNextHandle;
	uint64 fAllocated;
	uint64 fBound;
};

}
#endif
