/* SPDX-License-Identifier: MIT */
#include "BufferObject.h"

#include <string.h>

namespace IntelGfx {

BufferObject::BufferObject()
	: fArea(-1), fSize(0), fAddress(NULL), fGTT(NULL), fGraphicsAddress(0)
{
}

BufferObject::~BufferObject()
{
	Unbind();
	if (fArea >= 0)
		delete_area(fArea);
}

status_t
BufferObject::Init(size_t size)
{
	void* address = NULL;
	fArea = create_area("intel_gfx buffer", &address, B_ANY_KERNEL_ADDRESS,
		size, B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA
			| B_CLONEABLE_AREA);
	if (fArea < 0)
		return fArea;
	fSize = size;
	fAddress = address;
	memset(address, 0, size);
	return B_OK;
}

status_t
BufferObject::Bind(GlobalGTT& gtt)
{
	if (fArea < 0)
		return B_NO_INIT;
	if (fGTT != NULL)
		return B_BUSY;

	uint64 address = 0;
	status_t status = gtt.Bind(fArea, address);
	if (status != B_OK)
		return status;

	fGTT = &gtt;
	fGraphicsAddress = address;
	return B_OK;
}

status_t
BufferObject::Unbind()
{
	if (fGTT == NULL)
		return B_BAD_VALUE;

	status_t status = fGTT->Unbind(fGraphicsAddress, fSize);
	// The mapping is gone either way as far as this buffer is concerned;
	// keeping it would leak aperture space that nothing can reach again.
	fGTT = NULL;
	fGraphicsAddress = 0;
	return status;
}

}
