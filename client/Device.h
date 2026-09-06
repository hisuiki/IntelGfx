/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_DEVICE_H
#define INTEL_GFX_DEVICE_H

#include "IntelGfxABI.h"
#include <OS.h>

namespace IntelGfx {

class Device {
public:
	Device();
	~Device();
	status_t Open(const char* path);
	status_t GetInfo(DeviceInfo& info) const;
	status_t Create(uint64 size, CreateBuffer& request) const;
	status_t Close(uint32 handle) const;
	// Pins a buffer into the global GTT; the address is only meaningful while
	// the binding lasts. Fails with B_NOT_SUPPORTED without kGpuVirtualMemory.
	status_t Bind(uint32 handle, uint64& graphicsAddress) const;
	status_t Unbind(uint32 handle) const;
	// Runs the commands in a bound buffer and returns the fence they end on.
	status_t Submit(uint32 handle, uint64 offset, uint64 length,
		uint64& fence) const;
	status_t Wait(uint64 fence, bigtime_t timeout) const;
	status_t Status(EngineStatus& status) const;
	status_t Read(uint32 offset, uint32& value) const;
	int FD() const { return fFD; }
private:
	Device(const Device&) = delete;
	Device& operator=(const Device&) = delete;
	int fFD;
};

class MappedBuffer {
public:
	explicit MappedBuffer(Device& device);
	~MappedBuffer();
	status_t Init(uint64 size, bool bind = false);
	void* Address() const { return fAddress; }
	uint64 Size() const { return fSize; }
	uint32 Handle() const { return fHandle; }
	bool IsBound() const { return fBound; }
	uint64 GraphicsAddress() const { return fGraphicsAddress; }
private:
	MappedBuffer(const MappedBuffer&) = delete;
	MappedBuffer& operator=(const MappedBuffer&) = delete;
	Device& fDevice;
	uint32 fHandle;
	area_id fArea;
	void* fAddress;
	uint64 fSize;
	uint64 fGraphicsAddress;
	bool fBound;
};

}
#endif
