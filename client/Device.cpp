/* SPDX-License-Identifier: MIT */
#include "Device.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

namespace IntelGfx {

Device::Device() : fFD(-1) {}
Device::~Device() { if (fFD >= 0) close(fFD); }

status_t Device::Open(const char* path)
{
	if (fFD >= 0)
		return B_BUSY;
	fFD = open(path, O_RDWR | O_CLOEXEC);
	if (fFD < 0)
		return errno;
	DeviceInfo info;
	status_t status = GetInfo(info);
	if (status != B_OK) {
		close(fFD);
		fFD = -1;
	}
	return status;
}

status_t Device::GetInfo(DeviceInfo& info) const
{
	info = Request<DeviceInfo>();
	if (ioctl(fFD, kGetInfo, &info, sizeof(info)) < 0)
		return errno;
	if (info.header.version != kABIVersion || info.header.size != sizeof(info))
		return B_BAD_DATA;
	return B_OK;
}

status_t Device::Create(uint64 size, CreateBuffer& request) const
{
	request = Request<CreateBuffer>();
	request.size = size;
	return ioctl(fFD, kCreateBuffer, &request, sizeof(request)) < 0 ? errno : B_OK;
}

status_t Device::Close(uint32 handle) const
{
	CloseBuffer request = Request<CloseBuffer>();
	request.handle = handle;
	return ioctl(fFD, kCloseBuffer, &request, sizeof(request)) < 0 ? errno : B_OK;
}

status_t Device::Bind(uint32 handle, uint64& graphicsAddress) const
{
	BindBuffer request = Request<BindBuffer>();
	request.handle = handle;
	if (ioctl(fFD, kBindBuffer, &request, sizeof(request)) < 0)
		return errno;
	graphicsAddress = request.graphicsAddress;
	return B_OK;
}

status_t Device::Unbind(uint32 handle) const
{
	UnbindBuffer request = Request<UnbindBuffer>();
	request.handle = handle;
	return ioctl(fFD, kUnbindBuffer, &request, sizeof(request)) < 0 ? errno : B_OK;
}

status_t Device::Submit(uint32 handle, uint64 offset, uint64 length,
	uint64& fence, uint32 flags) const
{
	SubmitBatch request = Request<SubmitBatch>();
	request.handle = handle;
	request.flags = flags;
	request.offset = offset;
	request.length = length;
	if (ioctl(fFD, kSubmit, &request, sizeof(request)) < 0)
		return errno;
	fence = request.fence;
	return B_OK;
}

status_t Device::Wait(uint64 fence, bigtime_t timeout) const
{
	WaitFence request = Request<WaitFence>();
	request.fence = fence;
	request.timeout = (uint64)timeout;
	return ioctl(fFD, kWaitFence, &request, sizeof(request)) < 0 ? errno : B_OK;
}

status_t Device::CreateRenderContext(uint32& context) const
{
	CreateContext request = Request<CreateContext>();
	if (ioctl(fFD, kCreateContext, &request, sizeof(request)) < 0)
		return errno;
	context = request.context;
	return B_OK;
}

status_t Device::DestroyRenderContext(uint32 context) const
{
	DestroyContext request = Request<DestroyContext>();
	request.context = context;
	return ioctl(fFD, kDestroyContext, &request, sizeof(request)) < 0
		? errno : B_OK;
}

status_t Device::BindVirtual(uint32 handle, uint64 address) const
{
	IntelGfx::BindVirtual request = Request<IntelGfx::BindVirtual>();
	request.handle = handle;
	request.address = address;
	return ioctl(fFD, kBindVirtual, &request, sizeof(request)) < 0
		? errno : B_OK;
}

status_t Device::UnbindVirtual(uint32 handle) const
{
	UnbindBuffer request = Request<UnbindBuffer>();
	request.handle = handle;
	return ioctl(fFD, kUnbindVirtual, &request, sizeof(request)) < 0
		? errno : B_OK;
}

status_t Device::SubmitObjects(uint32 context, uint32 batchHandle,
	uint64 offset, uint64 length, const uint32* handles, uint32 count,
	uint64& fence) const
{
	IntelGfx::SubmitObjects request = Request<IntelGfx::SubmitObjects>();
	if (handles == NULL || count == 0 || count > kMaxBuffers)
		return B_BAD_VALUE;
	request.context = context;
	request.batchHandle = batchHandle;
	request.offset = offset;
	request.length = length;
	request.count = count;
	for (uint32 i = 0; i < count; i++)
		request.handles[i] = handles[i];
	if (ioctl(fFD, kSubmitObjects, &request, sizeof(request)) < 0)
		return errno;
	fence = request.fence;
	return B_OK;
}

status_t Device::WaitRender(uint64 fence, bigtime_t timeout) const
{
	WaitFence request = Request<WaitFence>();
	request.fence = fence;
	request.timeout = (uint64)timeout;
	return ioctl(fFD, kWaitRenderFence, &request, sizeof(request)) < 0
		? errno : B_OK;
}

status_t Device::Cache(uint32 handle, uint64 offset, uint64 length) const
{
	CacheBuffer request = Request<CacheBuffer>();
	request.handle = handle;
	request.offset = offset;
	request.length = length;
	return ioctl(fFD, kCacheBuffer, &request, sizeof(request)) < 0
		? errno : B_OK;
}

status_t Device::Status(EngineStatus& status, uint32 flags) const
{
	status = Request<EngineStatus>();
	status.statusBuffer[0] = flags;
	return ioctl(fFD, kEngineStatus, &status, sizeof(status)) < 0 ? errno : B_OK;
}

status_t Device::Read(uint32 offset, uint32& value) const
{
	ReadRegister request = Request<ReadRegister>();
	request.offset = offset;
	if (ioctl(fFD, kReadRegister, &request, sizeof(request)) < 0)
		return errno;
	value = request.value;
	return B_OK;
}

status_t Device::GetFramebuffer(Framebuffer& framebuffer) const
{
	framebuffer = Request<Framebuffer>();
	return ioctl(fFD, kFramebuffer, &framebuffer, sizeof(framebuffer)) < 0
		? errno : B_OK;
}

status_t Device::GetDisplayStatus(DisplayStatus& status) const
{
	status = Request<DisplayStatus>();
	return ioctl(fFD, kDisplayStatus, &status, sizeof(status)) < 0
		? errno : B_OK;
}

MappedBuffer::MappedBuffer(Device& device)
	: fDevice(device), fHandle(0), fArea(-1), fAddress(NULL), fSize(0),
	fGraphicsAddress(0), fBound(false) {}

MappedBuffer::~MappedBuffer()
{
	if (fArea >= 0)
		delete_area(fArea);
	if (fHandle != 0)
		fDevice.Close(fHandle);
}

status_t MappedBuffer::Init(uint64 size, bool bind)
{
	if (fHandle != 0)
		return B_BUSY;
	CreateBuffer request;
	status_t status = fDevice.Create(size, request);
	if (status != B_OK)
		return status;
	fHandle = request.handle;
	fSize = request.size;
	fArea = clone_area("IntelGfx client buffer", &fAddress, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, request.area);
	if (fArea < 0) {
		fDevice.Close(fHandle);
		fHandle = 0;
		return fArea;
	}
	if (bind) {
		// Closing the handle also drops the binding, so the failure path here
		// needs no separate unbind.
		status = fDevice.Bind(fHandle, fGraphicsAddress);
		if (status != B_OK) {
			delete_area(fArea);
			fArea = -1;
			fAddress = NULL;
			fDevice.Close(fHandle);
			fHandle = 0;
			return status;
		}
		fBound = true;
	}
	return B_OK;
}

}
