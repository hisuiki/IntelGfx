/* SPDX-License-Identifier: MIT */
// Kernel test fixture: exercises the PRODUCTION RenderClient and BufferObject
// through real Haiku ioctls and clone_area. No PCI device, MMIO or GPU emulation.
// This binary is not included in the IntelGfx HPKG.
#include "RenderClient.h"

#include <Drivers.h>
#include <KernelExport.h>
#include <new>
#include <string.h>

int32 api_version = B_CUR_DRIVER_API_VERSION;

static status_t Open(const char*, uint32, void** cookie)
{
	IntelGfx::DeviceInfo info = IntelGfx::Request<IntelGfx::DeviceInfo>();
	info.capabilities = IntelGfx::kCpuBuffers;
	info.maxBufferSize = IntelGfx::kMaxBufferSize;
	// No device, so no page table: binding must report kGpuVirtualMemory
	// absent and refuse every graphics address rather than inventing one.
	IntelGfx::RenderClient* client
		= new(std::nothrow) IntelGfx::RenderClient(NULL, info, NULL, NULL, NULL);
	if (client == NULL)
		return B_NO_MEMORY;
	*cookie = client;
	return B_OK;
}

static status_t Close(void*) { return B_OK; }
static status_t Free(void* cookie)
{
	delete (IntelGfx::RenderClient*)cookie;
	return B_OK;
}
static status_t Ioctl(void* cookie, uint32 operation, void* data, size_t size)
{
	return ((IntelGfx::RenderClient*)cookie)->Ioctl(operation, data, size);
}
static status_t Read(void*, off_t, void*, size_t* size)
{
	*size = 0;
	return B_NOT_ALLOWED;
}
static status_t Write(void*, off_t, const void*, size_t* size)
{
	*size = 0;
	return B_NOT_ALLOWED;
}

static device_hooks sHooks = {Open, Close, Free, Ioctl, Read, Write,
	NULL, NULL, NULL, NULL};

status_t init_hardware() { return B_OK; }
status_t init_driver() { return B_OK; }
void uninit_driver() {}
const char** publish_devices()
{
	static const char* devices[] = {"misc/intel_gfx_memory_test", NULL};
	return devices;
}
device_hooks* find_device(const char* name)
{
	return strcmp(name, "misc/intel_gfx_memory_test") == 0 ? &sHooks : NULL;
}
