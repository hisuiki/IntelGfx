/* SPDX-License-Identifier: MIT */
#include "RenderClient.h"

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

RenderClient::RenderClient(intel_info* device, const DeviceInfo& info,
	GlobalGTT* gtt, RenderEngine* blitter, RenderEngine* render)
	: fDevice(device), fInfo(info), fGTT(gtt), fNextHandle(1),
	fAllocated(0), fBound(0)
{
	fEngines[0] = blitter;
	fEngines[1] = render;

	mutex_init(&fLock, "intel_gfx client");
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

	if (_Engine(0) != NULL)
		fInfo.capabilities |= kRenderSubmission;
	else
		fInfo.capabilities &= ~(uint64)kRenderSubmission;
}

RenderClient::~RenderClient()
{
	// Each buffer drops its GTT mapping before its pages go away.
	for (uint32 i = 0; i < kMaxBuffers; i++)
		delete fBuffers[i];
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

status_t
RenderClient::Ioctl(uint32 operation, void* userBuffer, size_t length)
{
	MutexLocker locker(&fLock);
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
			BufferObject* buffer = _Find(request.handle);
			if (buffer == NULL)
				return B_ENTRY_NOT_FOUND;
			if (buffer->IsBound())
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
			request.fence = fence;
			return user_memcpy(userBuffer, &request, sizeof(request));
		}
		case kWaitFence: {
			WaitFence request;
			status_t status = ReadRequest(userBuffer, length, request);
			if (status != B_OK)
				return status;
			RenderEngine* engine = _Engine(0);
			if (engine == NULL)
				return B_NOT_SUPPORTED;
			if (request.timeout > 10000000)
				return B_BAD_VALUE;
			return engine->Wait(request.fence, (bigtime_t)request.timeout);
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

}
