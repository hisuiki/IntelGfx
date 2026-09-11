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
		RenderEngine* blitter, RenderEngine* render);
	~RenderClient();
	intel_info* Device() const { return fDevice; }
	status_t Ioctl(uint32 operation, void* userBuffer, size_t length);
private:
	BufferObject* _Find(uint32 handle, uint32* _slot = NULL) const;
	status_t _NativeIoctl(uint32 operation, void* userBuffer, size_t length);
	status_t _UnmapVirtual(uint32 slot);
	bool _IsCoherent() const;
	uint64 _GpuTicks() const;
	status_t _GpuActivity(void* userBuffer, size_t length);

	// Every open client of the device, so that one of them can report what the
	// others are asking the GPU to do.
	static RenderClient* sClients[kMaxActivityClients];
	static mutex sClientsLock;
	int32 fTeam;
	// What this client last reported, kept so that a report on it can still
	// be given while it is too busy to be asked. Guarded by sClientsLock.
	uint64 fReportedTicks;
	uint32 fReportedContexts;
	void _FlushObjects(const SubmitObjects& request);
	struct NativeState;
	NativeState* fNative;

	intel_info* fDevice;
	DeviceInfo fInfo;
	GlobalGTT* fGTT;
	RenderEngine* fEngines[2];
	RenderEngine* _Engine(uint32 flags) const;
	mutex fLock;
	BufferObject* fBuffers[kMaxBuffers];
	uint32 fHandles[kMaxBuffers];
	uint32 fNextHandle;
	uint64 fAllocated;
	uint64 fBound;
};

}
#endif
