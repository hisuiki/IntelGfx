/* SPDX-License-Identifier: MIT */
#include "RenderClient.h"
#include "Cache.h"

#include <team.h>

#include <new>
#include <string.h>
#include "intel_extreme_private.h"

#include <util/AutoLock.h>

namespace IntelGfx {

template<typename T> static status_t
ReadRequest(void* userBuffer, size_t length, T& request)
{
	if (length != sizeof(T))
		return B_BAD_VALUE;
	if (user_memcpy(&request, userBuffer, sizeof(T)) != B_OK)
		return B_BAD_ADDRESS;
	if (request.header.version != kABIVersion
		|| request.header.size != sizeof(T))
		return B_BAD_VALUE;
	return B_OK;
}

struct RenderClient::NativeState {
	PageTables vm;
	RenderEngine* contexts[kMaxContexts];
	uint32 ids[kMaxContexts];
	uint32 nextContext;
	uint64 addresses[kMaxBuffers];
	uint32 tiling[kMaxBuffers];
	uint32 stride[kMaxBuffers];
	uint64 completed;
	bool faulted;
	NativeState() : nextContext(1), completed(0), faulted(false)
	{
		memset(contexts, 0, sizeof(contexts));
		memset(ids, 0, sizeof(ids));
		memset(addresses, 0, sizeof(addresses));
		memset(tiling, 0, sizeof(tiling));
		memset(stride, 0, sizeof(stride));
	}
	~NativeState()
	{
		for (uint32 i = 0; i < kMaxContexts; i++)
			delete contexts[i];
	}
};

RenderClient* RenderClient::sClients[kMaxActivityClients];
mutex RenderClient::sClientsLock = MUTEX_INITIALIZER("intel_gfx clients");


// GPU time across this client's contexts, in command streamer ticks.
// Includes both native contexts (Iris/Mesa) and legacy per-client engines
// (kSubmit path), so every submission path is visible in the activity report.
uint64
RenderClient::_GpuTicks() const
{
	uint64 ticks = 0;
	if (fNative != NULL) {
		for (uint32 i = 0; i < kMaxContexts; i++) {
			if (fNative->contexts[i] != NULL)
				ticks += fNative->contexts[i]->GpuTicks();
		}
	}
	for (uint32 i = 0; i < B_COUNT_OF(fEngines); i++) {
		if (fEngines[i] != NULL)
			ticks += fEngines[i]->GpuTicks();
	}
	return ticks;
}


RenderClient::RenderClient(intel_info* device, const DeviceInfo& info,
	GlobalGTT* gtt, RenderEngine* blitter, RenderEngine* render)
	: fNative(NULL), fDevice(device), fInfo(info), fGTT(gtt), fNextHandle(1),
	fAllocated(0), fBound(0)
{
	fEngines[0] = NULL;
	fEngines[1] = NULL;
	RenderEngine* masters[2] = {blitter, render};
	for (uint32 i = 0; i < 2; i++) {
		if (gtt == NULL || masters[i] == NULL || !masters[i]->IsReady())
			continue;
		RenderEngine* context = new(std::nothrow) RenderEngine;
		if (context != NULL && context->InitClient(*gtt, *masters[i]) == B_OK)
			fEngines[i] = context;
		else
			delete context;
	}

	mutex_init(&fLock, "intel_gfx client");
	fTeam = team_get_current_team_id();
	{
		MutexLocker clients(&sClientsLock);
		for (uint32 i = 0; i < kMaxActivityClients; i++) {
			if (sClients[i] == NULL) {
				sClients[i] = this;
				break;
			}
		}
	}
	memset(fBuffers, 0, sizeof(fBuffers));
	memset(fHandles, 0, sizeof(fHandles));

	// The reported capability follows the address space this client actually
	// got, so it can never promise a graphics address the device cannot use.
	if (fGTT != NULL && fGTT->IsValid()) {
		fInfo.capabilities |= kGpuVirtualMemory;
		fInfo.graphicsAddressBase = fGTT->Base();
		fInfo.graphicsAddressSize = fGTT->Size();
	} else {
		fInfo.capabilities &= ~(uint64)kGpuVirtualMemory;
		fInfo.graphicsAddressBase = 0;
		fInfo.graphicsAddressSize = 0;
	}

	// Native rendering is deliberately limited to SKL/KBL, the platforms
	// whose logical context layout and cache setup this driver implements.
	if (info.graphicsVersion == 9 && _Engine(kUseRenderEngine) != NULL
		&& ((info.device & 0xff00) == 0x1900
			|| (info.device & 0xff00) == 0x5900))
		fInfo.capabilities |= kNativeRender;

	if (_Engine(0) != NULL)
		fInfo.capabilities |= kRenderSubmission;
	else
		fInfo.capabilities &= ~(uint64)kRenderSubmission;
}

RenderClient::~RenderClient()
{
	{
		MutexLocker clients(&sClientsLock);
		for (uint32 i = 0; i < kMaxActivityClients; i++) {
			if (sClients[i] == this)
				sClients[i] = NULL;
		}
	}

	// A timed-out GPU may still access every BO in its VM. Quarantine the
	// entire client until reboot instead of freeing DMA targets underneath it.
	bool faulted = fNative != NULL && fNative->faulted;
	for (uint32 i = 0; i < B_COUNT_OF(fEngines); i++)
		faulted |= fEngines[i] != NULL && fEngines[i]->IsFaulted();
	if (!faulted) {
		delete fNative;
		for (uint32 i = 0; i < kMaxBuffers; i++) {
			if (fBuffers[i] != NULL && fBuffers[i]->IsBound()) {
				for (uint32 e = 0; e < B_COUNT_OF(fEngines); e++) {
					if (fEngines[e] != NULL && fEngines[e]->IsReady())
						fEngines[e]->UnmapBuffer(fBuffers[i]->GraphicsAddress(),
							fBuffers[i]->Size());
				}
			}
			delete fBuffers[i];
		}
		for (uint32 i = 0; i < B_COUNT_OF(fEngines); i++)
			delete fEngines[i];
	} else
		dprintf("intel_gfx: quarantining faulted client DMA allocations\n");
	mutex_destroy(&fLock);
}

RenderEngine*
RenderClient::_Engine(uint32 flags) const
{
	RenderEngine* engine = fEngines[(flags & kUseRenderEngine) != 0 ? 1 : 0];
	if (engine == NULL || !engine->IsReady())
		return NULL;
	return engine;
}


BufferObject*
RenderClient::_Find(uint32 handle, uint32* _slot) const
{
	for (uint32 i = 0; i < kMaxBuffers; i++) {
		if (fBuffers[i] != NULL && fHandles[i] == handle) {
			if (_slot != NULL)
				*_slot = i;
			return fBuffers[i];
		}
	}
	return NULL;
}

// Whether the processor and the GPU already see each other's writes. Every
// part this driver grants a native context to is a Skylake or Kaby Lake with a
// last level cache the GPU shares, and the page attribute table entry all of
// its accesses resolve through is programmed write back and cached. Nothing
// the two exchange through memory needs the caches emptied by hand, which is
// what Linux relies on for the same parts.
bool
RenderClient::_IsCoherent() const
{
	return (fInfo.capabilities & kNativeRender) != 0;
}


// Writing back every byte of every buffer a batch names, before and after it
// runs, costs more than the batch does: a frame that references twenty
// megabytes of vertex and texture memory has all of it written back twice,
// which saturates a processor core and leaves the GPU waiting. It is only
// correct to skip on a coherent part, so it is only skipped there.
void
RenderClient::_FlushObjects(const SubmitObjects& request)
{
	if (_IsCoherent())
		return;
	for (uint32 i = 0; i < request.count; i++) {
		BufferObject* buffer = _Find(request.handles[i]);
		if (buffer != NULL)
			FlushCpuCache(buffer->Address(), buffer->Size());
	}
}


status_t
RenderClient::Ioctl(uint32 operation, void* userBuffer, size_t length)
{
	MutexLocker locker(&fLock);
	if (fNative != NULL && fNative->faulted && operation != kGetInfo
		&& operation != kEngineStatus)
		return B_DEV_NOT_READY;
	if (operation >= kCreateContext && operation < kOperationsEnd)
		return _NativeIoctl(operation, userBuffer, length);
	switch (operation) {
		case kGetInfo: {
			DeviceInfo request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			request = fInfo;
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
		case kCreateBuffer: {
			CreateBuffer request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.flags != 0 || request.reserved != 0 || request.size == 0
				|| request.size > kMaxBufferSize || fNextHandle == 0)
				return B_BAD_VALUE;
			uint64 size = (request.size + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);
			if (size > kClientMemoryLimit - fAllocated)
				return B_NO_MEMORY;
			uint32 slot = 0;
			while (slot < kMaxBuffers && fBuffers[slot] != NULL)
				slot++;
			if (slot == kMaxBuffers)
				return B_NO_MEMORY;
			BufferObject* buffer = new(std::nothrow) BufferObject;
			if (buffer == NULL)
				return B_NO_MEMORY;
			status = buffer->Init(size);
			if (status != B_OK) {
				delete buffer;
				return status;
			}
			request.size = size;
			request.handle = fNextHandle++;
			request.area = buffer->Area();
			status = user_memcpy(userBuffer, &request, sizeof(request));
			if (status != B_OK) {
				delete buffer;
				return status;
			}
			fBuffers[slot] = buffer;
			fHandles[slot] = request.handle;
			fAllocated += size;
			return B_OK;
		}
		case kCloseBuffer: {
			CloseBuffer request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.reserved != 0 || request.handle == 0)
				return B_BAD_VALUE;
			uint32 slot = 0;
			BufferObject* buffer = _Find(request.handle, &slot);
			if (buffer == NULL)
				return B_ENTRY_NOT_FOUND;
			if (buffer->IsBound()) {
				for (uint32 i = 0; i < B_COUNT_OF(fEngines); i++) {
					if (fEngines[i] != NULL && fEngines[i]->IsReady()) {
						fEngines[i]->UnmapBuffer(buffer->GraphicsAddress(),
							buffer->Size());
					}
				}
				fBound -= buffer->Size();
			}
			status = _UnmapVirtual(slot);
			if (status != B_OK)
				return status;
			fAllocated -= buffer->Size();
			delete buffer;
			fBuffers[slot] = NULL;
			fHandles[slot] = 0;
			return B_OK;
		}
		case kBindBuffer: {
			BindBuffer request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.flags != 0 || request.handle == 0
				|| request.graphicsAddress != 0)
				return B_BAD_VALUE;
			if (fGTT == NULL || !fGTT->IsValid())
				return B_NOT_SUPPORTED;
			uint32 slot;
			BufferObject* buffer = _Find(request.handle, &slot);
			if (buffer == NULL)
				return B_ENTRY_NOT_FOUND;
			if ((fNative != NULL && fNative->addresses[slot] != 0)
				|| buffer->IsBound())
				return B_BUSY;
			if (buffer->Size() > kClientApertureLimit - fBound)
				return B_NO_MEMORY;
			status = buffer->Bind(*fGTT);
			if (status != B_OK)
				return status;
			// The same address has to mean the same buffer to commands
			// running against either engine's own page tables.
			for (uint32 i = 0; i < B_COUNT_OF(fEngines); i++) {
				if (fEngines[i] == NULL || !fEngines[i]->IsReady())
					continue;
				status = fEngines[i]->MapBuffer(buffer->Area(),
					buffer->GraphicsAddress());
				if (status != B_OK) {
					buffer->Unbind();
					return status;
				}
			}
			request.graphicsAddress = buffer->GraphicsAddress();
			status = user_memcpy(userBuffer, &request, sizeof(request));
			if (status != B_OK) {
				buffer->Unbind();
				return status;
			}
			fBound += buffer->Size();
			return B_OK;
		}
		case kUnbindBuffer: {
			UnbindBuffer request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.reserved != 0 || request.handle == 0)
				return B_BAD_VALUE;
			BufferObject* buffer = _Find(request.handle);
			if (buffer == NULL)
				return B_ENTRY_NOT_FOUND;
			if (!buffer->IsBound())
				return B_BAD_VALUE;
			uint64 size = buffer->Size();
			uint64 address = buffer->GraphicsAddress();
			status = buffer->Unbind();
			for (uint32 i = 0; i < B_COUNT_OF(fEngines); i++) {
				if (fEngines[i] != NULL && fEngines[i]->IsReady())
					fEngines[i]->UnmapBuffer(address, size);
			}
			fBound -= size;
			return status;
		}
		case kSubmit: {
			SubmitBatch request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if ((request.flags & ~(uint32)kUseRenderEngine) != 0
				|| request.handle == 0
				|| request.fence != 0 || (request.offset & 0x3) != 0
				|| request.length == 0 || (request.length & 0x3) != 0)
				return B_BAD_VALUE;
			RenderEngine* engine = _Engine(request.flags);
			if (engine == NULL)
				return B_NOT_SUPPORTED;
			BufferObject* buffer = _Find(request.handle);
			if (buffer == NULL)
				return B_ENTRY_NOT_FOUND;
			if (!buffer->IsBound())
				return B_BAD_VALUE;
			if (request.offset >= buffer->Size()
				|| request.length > buffer->Size() - request.offset)
				return B_BAD_VALUE;
			uint64 fence = 0;
			status = engine->Submit(buffer->GraphicsAddress() + request.offset,
				(uint32)request.length, fence);
			if (status != B_OK)
				return status;
			request.fence = fence | ((uint64)(request.flags & kUseRenderEngine) << 32);
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
		case kWaitFence: {
			WaitFence request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if ((request.fence >> 32) > 1)
				return B_BAD_VALUE;
			RenderEngine* engine = _Engine((uint32)(request.fence >> 32));
			if (engine == NULL)
				return B_NOT_SUPPORTED;
			if (request.timeout > 10000000)
				return B_BAD_VALUE;
			return engine->Wait((uint32)request.fence, (bigtime_t)request.timeout);
		}
		case kReadRegister: {
			ReadRegister request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.value != 0 || (request.offset & 0x3) != 0)
				return B_BAD_VALUE;
			// The register window is the first two megabytes of the BAR; the
			// page tables above it are not registers and are not readable
			// this way.
			if (fDevice == NULL || request.offset >= 2 * 1024 * 1024)
				return B_BAD_VALUE;
			request.value = *(volatile uint32*)(fDevice->registers
				+ request.offset);
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
		case kFramebuffer: {
			Framebuffer request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (fDevice == NULL || fDevice->shared_info == NULL)
				return B_NOT_SUPPORTED;
			const intel_shared_info& shared = *fDevice->shared_info;
			if (shared.frame_buffer == 0 || shared.bytes_per_row == 0)
				return B_NOT_SUPPORTED;
			request.address = shared.frame_buffer_offset;
			request.pitch = shared.bytes_per_row;
			request.width = shared.current_mode.virtual_width;
			request.height = shared.current_mode.virtual_height;
			request.bitsPerPixel = shared.bits_per_pixel;
			// Commands run against the engine's page tables, so the
			// framebuffer has to be reachable there as well.
			for (uint32 i = 0; i < B_COUNT_OF(fEngines) && fGTT != NULL; i++) {
				if (fEngines[i] == NULL || !fEngines[i]->IsReady())
					continue;
				status = fEngines[i]->MapGlobalRange(*fGTT, request.address,
					(uint64)request.pitch * request.height);
				if (status != B_OK)
					return status;
			}
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
		case kDisplayStatus: {
			DisplayStatus request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (fDevice == NULL)
				return B_NOT_SUPPORTED;
			request.vblankCount = fDevice->vblank_count;
			request.masterInterrupt = *(volatile uint32*)(fDevice->registers
				+ 0x44200);
			for (uint32 pipe = 0; pipe < 3; pipe++) {
				request.pipeInterruptEnable[pipe]
					= *(volatile uint32*)(fDevice->registers
						+ 0x4440c + 0x10 * pipe);
				request.pipeInterruptMask[pipe]
					= *(volatile uint32*)(fDevice->registers
						+ 0x44404 + 0x10 * pipe);
				request.frameCount[pipe]
					= *(volatile uint32*)(fDevice->registers
						+ 0x70040 + 0x1000 * pipe);
			}
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
		case kEngineStatus: {
			EngineStatus request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			// The reserved field selects the engine, so that either can be
			// asked what it is doing.
			RenderEngine* engine = _Engine(request.statusBuffer[0]);
			if (engine == NULL)
				return B_NOT_SUPPORTED;
			status = engine->Status(request);
			if (status != B_OK)
				return status;
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
		default:
			return B_DEV_INVALID_IOCTL;
	}
}

status_t
RenderClient::_UnmapVirtual(uint32 slot)
{
	if (fNative == NULL)
		return B_OK;
	if (fNative->addresses[slot] != 0) {
		status_t status = fNative->vm.Unmap(fNative->addresses[slot],
			fBuffers[slot]->Size());
		if (status != B_OK)
			return status;
		fNative->addresses[slot] = 0;
	}
	fNative->tiling[slot] = kLinear;
	fNative->stride[slot] = 0;
	return B_OK;
}

status_t
RenderClient::_NativeIoctl(uint32 operation, void* userBuffer, size_t length)
{
	if ((fInfo.capabilities & kNativeRender) == 0)
		return B_NOT_SUPPORTED;
	if (fNative == NULL) {
		fNative = new(std::nothrow) NativeState;
		if (fNative == NULL)
			return B_NO_MEMORY;
		status_t status = fNative->vm.Init();
		if (status != B_OK) {
			delete fNative;
			fNative = NULL;
			return status;
		}
	}
	switch (operation) {
		case kCreateContext: {
			CreateContext request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.context != 0 || request.flags != 0)
				return B_BAD_VALUE;
			uint32 slot = 0;
			while (slot < kMaxContexts && fNative->contexts[slot] != NULL)
				slot++;
			if (slot == kMaxContexts || fNative->nextContext == 0)
				return B_NO_MEMORY;
			RenderEngine* context = new(std::nothrow) RenderEngine;
			if (context == NULL)
				return B_NO_MEMORY;
			status = context->InitClient(*fGTT, *fEngines[1], &fNative->vm);
			if (status == B_OK) {
				request.context = fNative->nextContext++;
				status = user_memcpy(userBuffer, &request, sizeof(request));
			}
			if (status != B_OK) {
				delete context;
				return status;
			}
			fNative->contexts[slot] = context;
			fNative->ids[slot] = request.context;
			return B_OK;
		}
		case kDestroyContext: {
			DestroyContext request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.context == 0 || request.reserved != 0)
				return B_BAD_VALUE;
			for (uint32 i = 0; i < kMaxContexts; i++) {
				if (fNative->ids[i] == request.context) {
					delete fNative->contexts[i];
					fNative->contexts[i] = NULL;
					fNative->ids[i] = 0;
					return B_OK;
				}
			}
			return B_ENTRY_NOT_FOUND;
		}
		case kBindVirtual: {
			BindVirtual request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.flags != 0 || request.handle == 0
				|| request.address == 0 || request.address >= kVirtualAddressLimit
				|| (request.address & (B_PAGE_SIZE - 1)) != 0)
				return B_BAD_VALUE;
			uint32 slot;
			BufferObject* buffer = _Find(request.handle, &slot);
			if (buffer == NULL)
				return B_ENTRY_NOT_FOUND;
			if (buffer->Size() > kVirtualAddressLimit - request.address)
				return B_BAD_VALUE;
			if (fNative->addresses[slot] == request.address)
				return B_OK;
			if (fNative->addresses[slot] != 0 || buffer->IsBound())
				return B_BUSY;
			for (uint32 i = 0; i < kMaxBuffers; i++) {
				uint64 address = fNative->addresses[i];
				if (address != 0 && request.address < address + fBuffers[i]->Size()
					&& address < request.address + buffer->Size())
					return B_BUSY;
			}
			status = fNative->vm.Map(buffer->Area(), request.address);
			if (status != B_OK) {
				fNative->vm.Unmap(request.address, buffer->Size());
				return status;
			}
			fNative->addresses[slot] = request.address;
			return B_OK;
		}
		case kUnbindVirtual: {
			UnbindBuffer request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.reserved != 0 || request.handle == 0)
				return B_BAD_VALUE;
			uint32 slot;
			if (_Find(request.handle, &slot) == NULL)
				return B_ENTRY_NOT_FOUND;
			return _UnmapVirtual(slot);
		}
		case kBufferLayout: {
			BufferLayout request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.reserved != 0 || request.tiling > kYTile
				|| (request.tiling != kLinear && (request.stride == 0
					|| request.stride % (request.tiling == kXTile ? 512 : 128) != 0)))
				return B_BAD_VALUE;
			uint32 slot;
			BufferObject* buffer = _Find(request.handle, &slot);
			if (buffer == NULL)
				return B_ENTRY_NOT_FOUND;
			if (request.stride > buffer->Size())
				return B_BAD_VALUE;
			fNative->tiling[slot] = request.tiling;
			fNative->stride[slot] = request.stride;
			return B_OK;
		}
		case kGpuActivity: {
			GpuActivity request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			// The total covers every engine, so blitter-only workloads are
			// visible too. Each engine's TotalGpuTicks is the sum across
			// every context that has ever run on that engine's scheduler.
			request = Request<GpuActivity>();
			request.timestampHz = kGpuTimestampHz;
			request.totalTicks = 0;
			bool hasEngine = false;
			for (uint32 i = 0; i < B_COUNT_OF(fEngines); i++) {
				if (fEngines[i] != NULL && fEngines[i]->IsReady()) {
					request.totalTicks += fEngines[i]->TotalGpuTicks();
					hasEngine = true;
				}
			}
			if (!hasEngine)
				return B_NOT_SUPPORTED;
			// Reporting on the other clients needs their locks, and this one
			// already holds its own, so only ever take theirs after it.
			MutexLocker clients(&sClientsLock);
			for (uint32 i = 0; i < kMaxActivityClients; i++) {
				RenderClient* client = sClients[i];
				if (client == NULL)
					continue;
				uint64 ticks;
				uint32 contexts = 0;
				if (client == this) {
					ticks = _GpuTicks();
					if (fNative != NULL) {
						for (uint32 c = 0; c < kMaxContexts; c++) {
							if (fNative->contexts[c] != NULL)
								contexts++;
						}
					}
				} else {
					MutexLocker other(&client->fLock);
					ticks = client->_GpuTicks();
					if (client->fNative != NULL) {
						for (uint32 c = 0; c < kMaxContexts; c++) {
							if (client->fNative->contexts[c] != NULL)
								contexts++;
						}
					}
				}
				// A process can open the render node more than once (Vulkan does
				// this routinely). Report one accumulated entry per team so every
				// consumer sees processes rather than driver file handles.
				ActivityClient* entry = NULL;
				for (uint32 j = 0; j < request.count; j++) {
					if (request.clients[j].team == client->fTeam) {
						entry = &request.clients[j];
						break;
					}
				}
				if (entry == NULL) {
					entry = &request.clients[request.count++];
					entry->team = client->fTeam;
				}
				entry->contexts += contexts;
				entry->ticks += ticks;
			}
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
		case kGetTopology: {
			Topology request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			RenderEngine* engine = _Engine(kUseRenderEngine);
			if (engine == NULL)
				return B_NOT_SUPPORTED;
			request = engine->ShaderTopology();
			request.header.version = kABIVersion;
			request.header.size = sizeof(request);
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
		case kCacheBuffer: {
			CacheBuffer request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.reserved != 0)
				return B_BAD_VALUE;
			BufferObject* buffer = _Find(request.handle);
			if (buffer == NULL)
				return B_ENTRY_NOT_FOUND;
			if (request.offset > buffer->Size()
				|| request.length > buffer->Size() - request.offset)
				return B_BAD_VALUE;
			if (!_IsCoherent()) {
				FlushCpuCache((uint8*)buffer->Address() + request.offset,
					request.length);
			}
			return B_OK;
		}
		case kWaitRenderFence: {
			WaitFence request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.fence == 0 || request.fence > fNative->completed
				|| request.timeout > 10000000)
				return B_BAD_VALUE;
			return B_OK;
		}
		case kSubmitObjects: {
			SubmitObjects request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			if (request.flags != 0 || request.fence != 0 || request.context == 0
				|| request.count == 0 || request.count > kMaxBuffers
				|| request.length == 0 || (request.offset & 7) != 0
				|| (request.length & 3) != 0)
				return B_BAD_VALUE;
			RenderEngine* context = NULL;
			for (uint32 i = 0; i < kMaxContexts; i++) {
				if (fNative->ids[i] == request.context)
					context = fNative->contexts[i];
			}
			if (context == NULL)
				return B_ENTRY_NOT_FOUND;
			uint32 batchSlot;
			BufferObject* batch = _Find(request.batchHandle, &batchSlot);
			if (batch == NULL)
				return B_ENTRY_NOT_FOUND;
			if (request.offset >= batch->Size()
				|| request.length > batch->Size() - request.offset)
				return B_BAD_VALUE;
			bool includesBatch = false;
			for (uint32 i = 0; i < request.count; i++) {
				uint32 slot;
				BufferObject* buffer = _Find(request.handles[i], &slot);
				if (buffer == NULL || fNative->addresses[slot] == 0)
					return B_BAD_VALUE;
				for (uint32 j = 0; j < i; j++) {
					if (request.handles[j] == request.handles[i])
						return B_BAD_VALUE;
				}
				includesBatch |= request.handles[i] == request.batchHandle;
			}
			if (!includesBatch)
				return B_BAD_VALUE;
			// No user metadata is re-read after validation. BOs and the VM
			// remain owned by this locked client throughout the submission.
			_FlushObjects(request);
			uint64 hardwareFence;
			status = context->Submit(fNative->addresses[batchSlot] + request.offset,
				(uint32)request.length, hardwareFence);
			if (status != B_OK) {
				fNative->faulted = context->IsFaulted();
				return status;
			}
			_FlushObjects(request);
			request.fence = ++fNative->completed;
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
	}
	return B_DEV_INVALID_IOCTL;
}

}
