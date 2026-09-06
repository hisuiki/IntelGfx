/* SPDX-License-Identifier: MIT */
#include "Device.h"
#include "GpuHardware.h"
#include "DeviceRoster.h"
#include "ServerProtocol.h"

#include <Application.h>
#include <Message.h>
#include <Messenger.h>
#include <Notification.h>
#include <Screen.h>
#include <String.h>
#include <new>
#include <errno.h>
#include <stdio.h>
#include <string.h>

using namespace IntelGfx;

static void PrintInfo(const DeviceInfo& info)
{
	printf("IntelGfx ABI %" B_PRIu32 ": %04x:%04x revision %02x at %02x:%02x.%u\n",
		info.header.version, info.vendor, info.device, info.revision,
		info.bus, info.slot, info.function);
	printf("Graphics version: %" B_PRIu32 "; capabilities: 0x%" B_PRIx64 "\n",
		info.graphicsVersion, info.capabilities);
	if ((info.capabilities & kGpuVirtualMemory) != 0) {
		printf("Graphics addresses: %" B_PRIu64 " MiB at 0x%" B_PRIx64 "\n",
			info.graphicsAddressSize >> 20, info.graphicsAddressBase);
	} else
		puts("Graphics addresses: unavailable");
	printf("GPU submission: %s\n",
		(info.capabilities & kRenderSubmission) != 0 ? "available" : "not implemented");
}

static status_t PrintDevices(BMessage& reply)
{
	BMessage item;
	int32 count = 0;
	while (reply.FindMessage("device", count++, &item) == B_OK) {
		const char* path = NULL;
		int32 status;
		if (item.FindString("path", &path) != B_OK
			|| item.FindInt32("status", &status) != B_OK)
			return B_BAD_DATA;
		printf("%s: %s\n", path, strerror(status));
		if (status != B_OK)
			continue;
		const void* data;
		ssize_t size;
		if (item.FindData("info", B_RAW_TYPE, &data, &size) != B_OK
			|| size != sizeof(DeviceInfo))
			return B_BAD_DATA;
		DeviceInfo info;
		memcpy(&info, data, sizeof(info));
		PrintInfo(info);
	}
	if (count == 1)
		puts("No Intel display devices found.");
	return B_OK;
}

// A graphics address has to be page aligned and cover the whole buffer
// inside the range the driver said it hands out.
static bool InRange(const DeviceInfo& info, uint64 address, uint64 size)
{
	return (address & (B_PAGE_SIZE - 1)) == 0
		&& address >= info.graphicsAddressBase
		&& address - info.graphicsAddressBase < info.graphicsAddressSize
		&& size <= info.graphicsAddressSize
			- (address - info.graphicsAddressBase);
}


// Exercises global GTT binding: address sanity, handle ownership, the
// per-client aperture budget, and that unbinding really returns the space.
static status_t GttTest(Device& device, const char* path)
{
	DeviceInfo info;
	status_t status = device.GetInfo(info);
	if (status != B_OK)
		return status;
	uint64 address = 0;
	if ((info.capabilities & kGpuVirtualMemory) == 0) {
		// A device that reports no aperture must also refuse to hand out an
		// address, rather than leaving the ioctl to answer for itself.
		CreateBuffer probe;
		status = device.Create(B_PAGE_SIZE, probe);
		if (status == B_OK) {
			if (device.Bind(probe.handle, address) != B_NOT_SUPPORTED)
				status = B_ERROR;
			device.Close(probe.handle);
		}
		if (status != B_OK)
			return status;
		fputs("This device reports no global GTT; binding is refused.\n", stderr);
		return B_NOT_SUPPORTED;
	}

	if (device.Bind(0, address) != B_BAD_VALUE
		|| device.Bind(0xffffffff, address) != B_ENTRY_NOT_FOUND)
		return B_ERROR;

	CreateBuffer request;
	status = device.Create(128 * 1024, request);
	if (status != B_OK)
		return status;
	// A buffer that was never bound cannot be unbound.
	if (device.Unbind(request.handle) != B_BAD_VALUE)
		return B_ERROR;
	status = device.Bind(request.handle, address);
	if (status != B_OK)
		return status;
	if (!InRange(info, address, request.size))
		return B_ERROR;
	uint64 second = 0;
	if (device.Bind(request.handle, second) != B_BUSY)
		return B_ERROR;

	Device other;
	status = other.Open(path);
	if (status != B_OK)
		return status;
	if (other.Unbind(request.handle) != B_ENTRY_NOT_FOUND)
		return B_ERROR;

	status = device.Unbind(request.handle);
	if (status != B_OK)
		return status;
	if (device.Unbind(request.handle) != B_BAD_VALUE
		|| device.Close(request.handle) != B_OK)
		return B_ERROR;

	// The budget is enforced, and closing a bound buffer gives its aperture
	// space back: the same run has to fit twice over.
	const uint64 chunk = 8 * 1024 * 1024;
	const uint32 fitting = (uint32)(kClientApertureLimit / chunk);
	if (fitting == 0 || fitting >= 63)
		return B_ERROR;
	for (int pass = 0; pass < 2; pass++) {
		MappedBuffer* buffers[64] = {};
		status = B_OK;
		for (uint32 i = 0; i <= fitting; i++) {
			buffers[i] = new(std::nothrow) MappedBuffer(device);
			if (buffers[i] == NULL) {
				status = B_NO_MEMORY;
				break;
			}
			status_t bound = buffers[i]->Init(chunk, true);
			if (i == fitting) {
				// One chunk past the budget has to be refused, not granted.
				status = bound == B_NO_MEMORY ? B_OK : B_ERROR;
				break;
			}
			if (bound != B_OK) {
				status = bound;
				break;
			}
			uint8* bytes = (uint8*)buffers[i]->Address();
			if (!buffers[i]->IsBound()
				|| !InRange(info, buffers[i]->GraphicsAddress(), chunk)
				|| bytes[chunk - 1] != 0) {
				status = B_ERROR;
				break;
			}
			bytes[chunk - 1] = (uint8)i;
		}
		// Closing the bound buffers must hand every byte of that aperture
		// space back, or the second pass cannot fit the same run again.
		for (uint32 i = 0; i < 64; i++)
			delete buffers[i];
		if (status != B_OK)
			return status;
	}

	puts("PASS: graphics addresses, rebinding, handle ownership, aperture budget.");
	puts("This test does not submit GPU commands.");
	return B_OK;
}


static status_t BufferTest(Device& device, const char* path)
{
	CreateBuffer request;
	if (device.Create(0, request) == B_OK
		|| device.Create(kMaxBufferSize + 1, request) == B_OK)
		return B_ERROR;
	Device other;
	status_t status = other.Open(path);
	if (status != B_OK)
		return status;
	status = device.Create(1, request);
	if (status != B_OK)
		return status;
	if (request.size != B_PAGE_SIZE || other.Close(request.handle) != B_ENTRY_NOT_FOUND)
		return B_ERROR;
	area_id area;
	void* address = NULL;
	area = clone_area("IntelGfx lifetime test", &address, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, request.area);
	if (area < 0)
		return area;
	status = device.Close(request.handle);
	if (status == B_OK && device.Close(request.handle) != B_ENTRY_NOT_FOUND)
		status = B_ERROR;
	// A retained clone must remain valid after closing its kernel handle.
	if (status == B_OK) {
		memset(address, 0x5a, B_PAGE_SIZE);
		if (((uint8*)address)[B_PAGE_SIZE - 1] != 0x5a)
			status = B_ERROR;
	}
	delete_area(area);
	if (status != B_OK)
		return status;
	for (int iteration = 0; iteration < 128; iteration++) {
		MappedBuffer buffer(device);
		status = buffer.Init(65537);
		if (status != B_OK)
			return status;
		uint8* bytes = (uint8*)buffer.Address();
		for (uint64 i = 0; i < buffer.Size(); i++) {
			if (bytes[i] != 0)
				return B_BAD_DATA;
			bytes[i] = (uint8)(i ^ iteration);
		}
		for (uint64 i = 0; i < buffer.Size(); i++) {
			if (bytes[i] != (uint8)(i ^ iteration))
				return B_BAD_DATA;
		}
	}
	puts("PASS: bounds, per-open handles, clone lifetime, zeroing, CPU read/write.");
	puts("This test does not submit GPU commands.");
	return B_OK;
}

static void PrintEngineStatus(const EngineStatus& status)
{
	printf("  ring head %#" B_PRIx32 " tail %#" B_PRIx32 " start %#" B_PRIx32
		" ctl %#" B_PRIx32 "\n", status.ringHead, status.ringTail,
		status.ringStart, status.ringControl);
	printf("  active head %#" B_PRIx32 " instruction %#" B_PRIx32
		" error %#" B_PRIx32 "\n", status.activeHead,
		status.instructionHeader, status.errorIdentity);
	printf("  mi mode %#" B_PRIx32 " mode %#" B_PRIx32 " status page %#"
		B_PRIx32 "\n", status.miMode, status.mode,
		status.hardwareStatusAddress);
	printf("  execlist status %#" B_PRIx32 ":%#" B_PRIx32 " pointer %#"
		B_PRIx32 " gt interrupts %#" B_PRIx32 "\n", status.execlistStatusHigh,
		status.execlistStatusLow, status.statusPointer,
		status.interruptStatus);
	printf("  context head %#" B_PRIx32 " tail %#" B_PRIx32 " start %#"
		B_PRIx32 " ctl %#" B_PRIx32 "\n", status.contextRingHead,
		status.contextRingTail, status.contextRingStart,
		status.contextRingControl);
	printf("  context control %#" B_PRIx32 ", engine rewrote %" B_PRIu32
		" dwords from %" B_PRIu32 ", ring[0] %#" B_PRIx32 "\n",
		status.contextControl, status.contextChanged,
		status.contextFirstChange, status.ringFirstDword);
	printf("  fence %" B_PRIu32 " of %" B_PRIu32 "\n", status.fence,
		status.submitted);
	printf("  context events");
	for (uint32 i = 0; i < 12; i += 2) {
		printf(" %#" B_PRIx32 ":%#" B_PRIx32, status.statusBuffer[i + 1],
			status.statusBuffer[i]);
	}
	printf("\n");
}


// Runs a batch on the GPU that stores a known value into another buffer, and
// checks that the value arrived. This is the whole path: page tables, the
// context, the ring, the execution list port and the fence.
static status_t SubmitTest(Device& device)
{
	DeviceInfo info;
	status_t status = device.GetInfo(info);
	if (status != B_OK)
		return status;
	if ((info.capabilities & kRenderSubmission) == 0) {
		fputs("This device reports no GPU submission; nothing to test.\n",
			stderr);
		return B_NOT_SUPPORTED;
	}

	MappedBuffer target(device);
	status = target.Init(B_PAGE_SIZE, true);
	if (status != B_OK)
		return status;

	MappedBuffer batch(device);
	status = batch.Init(B_PAGE_SIZE, true);
	if (status != B_OK)
		return status;

	const uint32 kValue = 0x1ce1ce;
	uint64 address = target.GraphicsAddress();
	uint32* commands = (uint32*)batch.Address();
	// The batch runs against the engine's own page tables, where a buffer
	// sits at the same address its global mapping uses.
	commands[0] = kMiStoreDataImmediate;
	commands[1] = (uint32)address;
	commands[2] = (uint32)(address >> 32);
	commands[3] = kValue;
	commands[4] = kMiBatchBufferEnd;

	uint64 fence = 0;
	status = device.Submit(batch.Handle(), 0, 5 * sizeof(uint32), fence);
	if (status != B_OK)
		return status;
	printf("Submitted; fence %" B_PRIu64 " at graphics address 0x%" B_PRIx64
		"\n", fence, batch.GraphicsAddress());

	status = device.Wait(fence, 2000000);
	if (status != B_OK) {
		fprintf(stderr, "The GPU did not signal the fence: %s\n",
			strerror(status));
		EngineStatus engine;
		if (device.Status(engine) == B_OK) {
			puts("Engine afterwards:");
			PrintEngineStatus(engine);
		}
		return status;
	}

	uint32 written = *(volatile uint32*)target.Address();
	if (written != kValue) {
		fprintf(stderr, "Fence signalled but the buffer holds 0x%" B_PRIx32
			", not 0x%" B_PRIx32 "\n", written, kValue);
		return B_ERROR;
	}

	puts("PASS: the GPU ran the commands and the write is visible.");
	return B_OK;
}


// What the display side of the hardware reports about its outputs, read
// straight from the registers rather than from what the accelerant decided
// at boot. Hot plug status is live, so this also answers whether a screen is
// attached right now.
static status_t Displays(Device& device)
{
	static const struct { const char* name; uint32 hotplug; uint32 buffer; }
	kPorts[] = {
		{ "DDI A (built in panel)", 1 << 24, 0x64000 },
		{ "DDI B", 1 << 21, 0x64100 },
		{ "DDI C", 1 << 22, 0x64200 },
		{ "DDI D", 1 << 23, 0x64300 },
		{ "DDI E", 1 << 25, 0x64400 },
	};

	uint32 hotplug = 0;
	status_t status = device.Read(0xc4000, hotplug);	// SDEISR
	if (status != B_OK)
		return status;
	printf("South display interrupt status: %#" B_PRIx32 "\n", hotplug);

	for (size_t i = 0; i < sizeof(kPorts) / sizeof(kPorts[0]); i++) {
		uint32 buffer = 0;
		status = device.Read(kPorts[i].buffer, buffer);
		if (status != B_OK)
			return status;
		printf("%-24s %s, buffer control %#" B_PRIx32 " (%s)\n",
			kPorts[i].name,
			(hotplug & kPorts[i].hotplug) != 0 ? "screen attached"
				: "nothing attached",
			buffer, (buffer & 1) != 0 ? "enabled" : "disabled");
	}
	return B_OK;
}


// Fills a rectangle of the visible screen with the blitter, which is the
// first thing this driver does that anyone can see happening.
static status_t FillTest(Device& device)
{
	DeviceInfo info;
	status_t status = device.GetInfo(info);
	if (status != B_OK)
		return status;
	if ((info.capabilities & kRenderSubmission) == 0) {
		fputs("This device reports no GPU submission.\n", stderr);
		return B_NOT_SUPPORTED;
	}

	Framebuffer screen;
	status = device.GetFramebuffer(screen);
	if (status != B_OK)
		return status;
	printf("Framebuffer %" B_PRIu32 "x%" B_PRIu32 " at 0x%" B_PRIx64
		", pitch %" B_PRIu32 ", %" B_PRIu32 " bits per pixel\n",
		screen.width, screen.height, screen.address, screen.pitch,
		screen.bitsPerPixel);
	if (screen.bitsPerPixel != 32) {
		fputs("This test only knows 32 bit colour.\n", stderr);
		return B_NOT_SUPPORTED;
	}

	// A band across the middle of the screen, wide enough to be unmistakable
	// and short enough to leave the desktop usable.
	uint32 x = screen.width / 4;
	uint32 y = screen.height / 3;
	uint32 width = screen.width / 2;
	uint32 height = screen.height / 6;
	const uint32 kColour = 0x00d8402f;		// a red the desktop does not use

	MappedBuffer batch(device);
	status = batch.Init(B_PAGE_SIZE, true);
	if (status != B_OK)
		return status;

	uint32* commands = (uint32*)batch.Address();
	commands[0] = kXyColorBlit | kBlitWriteRgb | kBlitWriteAlpha;
	commands[1] = kBlitDepth32 | kBlitRopColorCopy | screen.pitch;
	commands[2] = (y << 16) | x;
	commands[3] = ((y + height) << 16) | (x + width);
	commands[4] = (uint32)screen.address;
	commands[5] = (uint32)(screen.address >> 32);
	commands[6] = kColour;
	commands[7] = kMiBatchBufferEnd;

	uint64 fence = 0;
	status = device.Submit(batch.Handle(), 0, 8 * sizeof(uint32), fence);
	if (status != B_OK)
		return status;

	status = device.Wait(fence, 2000000);
	if (status != B_OK) {
		fprintf(stderr, "The GPU did not signal the fence: %s\n",
			strerror(status));
		EngineStatus engine;
		if (device.Status(engine) == B_OK) {
			puts("Engine afterwards:");
			PrintEngineStatus(engine);
		}
		return status;
	}

	// Read the result back with the GPU as well: copying a piece of the
	// screen into memory this process can look at says whether the fill
	// really landed, without anyone having to watch the display.
	const uint32 kCheckWidth = 16;
	const uint32 kCheckHeight = 4;
	MappedBuffer readback(device);
	status = readback.Init(B_PAGE_SIZE, true);
	if (status != B_OK)
		return status;

	uint64 target = readback.GraphicsAddress();
	commands[0] = kXySourceCopyBlit | kBlitWriteRgb | kBlitWriteAlpha;
	commands[1] = kBlitDepth32 | kBlitRopSourceCopy | (kCheckWidth * 4);
	commands[2] = 0;
	commands[3] = (kCheckHeight << 16) | kCheckWidth;
	commands[4] = (uint32)target;
	commands[5] = (uint32)(target >> 32);
	commands[6] = ((y + height / 2) << 16) | (x + width / 2);
	commands[7] = screen.pitch;
	commands[8] = (uint32)screen.address;
	commands[9] = (uint32)(screen.address >> 32);
	commands[10] = kMiBatchBufferEnd;

	status = device.Submit(batch.Handle(), 0, 11 * sizeof(uint32), fence);
	if (status != B_OK)
		return status;
	status = device.Wait(fence, 2000000);
	if (status != B_OK) {
		fprintf(stderr, "The copy back never completed: %s\n",
			strerror(status));
		return status;
	}

	const uint32* pixels = (const uint32*)readback.Address();
	for (uint32 i = 0; i < kCheckWidth * kCheckHeight; i++) {
		if ((pixels[i] & 0x00ffffff) != kColour) {
			fprintf(stderr, "Pixel %" B_PRIu32 " reads %#" B_PRIx32
				", expected %#" B_PRIx32 "\n", i, pixels[i], kColour);
			return B_ERROR;
		}
	}

	printf("PASS: the GPU filled %" B_PRIu32 "x%" B_PRIu32 " at %" B_PRIu32
		",%" B_PRIu32 " on screen and read the same pixels back.\n",
		width, height, x, y);
	return B_OK;
}


// How long one submission takes from end to end, which decides whether the
// accelerant should hand small operations to the GPU at all.
static status_t Bench(Device& device)
{
	MappedBuffer batch(device);
	status_t status = batch.Init(B_PAGE_SIZE, true);
	if (status != B_OK)
		return status;

	uint32* commands = (uint32*)batch.Address();
	commands[0] = kMiBatchBufferEnd;
	commands[1] = kMiNoop;

	const uint32 kRounds = 200;
	bigtime_t start = system_time();
	for (uint32 i = 0; i < kRounds; i++) {
		uint64 fence = 0;
		status = device.Submit(batch.Handle(), 0, 2 * sizeof(uint32), fence);
		if (status != B_OK)
			return status;
		status = device.Wait(fence, 2000000);
		if (status != B_OK)
			return status;
	}
	bigtime_t elapsed = system_time() - start;

	printf("%" B_PRIu32 " submissions in %" B_PRIdBIGTIME " us, %"
		B_PRIdBIGTIME " us each\n", kRounds, elapsed, elapsed / kRounds);
	return B_OK;
}


// Watches the display interrupt for a second, which is long enough to tell
// sixty vertical blanks from none at all.
static status_t Vblank(Device& device)
{
	DisplayStatus first;
	status_t status = device.GetDisplayStatus(first);
	if (status != B_OK)
		return status;

	printf("Master interrupt control: %#" B_PRIx32 " (%s)\n",
		first.masterInterrupt,
		(first.masterInterrupt & (1u << 31)) != 0 ? "enabled" : "disabled");
	for (uint32 pipe = 0; pipe < 3; pipe++) {
		printf("Pipe %c: interrupts enabled %#" B_PRIx32 ", masked %#" B_PRIx32
			", frame %" B_PRIu32 "\n", 'A' + (char)pipe,
			first.pipeInterruptEnable[pipe], first.pipeInterruptMask[pipe],
			first.frameCount[pipe]);
	}

	snooze(1000000);

	DisplayStatus second;
	status = device.GetDisplayStatus(second);
	if (status != B_OK)
		return status;

	printf("In one second: %" B_PRIu64 " vertical blanks", 
		second.vblankCount - first.vblankCount);
	for (uint32 pipe = 0; pipe < 3; pipe++) {
		uint32 frames = second.frameCount[pipe] - first.frameCount[pipe];
		if (frames != 0)
			printf(", pipe %c drew %" B_PRIu32 " frames", 'A' + (char)pipe, frames);
	}
	printf("\n");

	if (second.vblankCount == first.vblankCount)
		puts("The display is not interrupting: nothing can wait for a frame.");
	return B_OK;
}


// The panel backlight, through the same interface anything else on the
// system uses, so that this says whether brightness works for applications
// rather than only whether the register can be written.
static status_t Brightness(const char* value)
{
	BApplication application("application/x-vnd.IntelGfx-ctl");
	BScreen screen(B_MAIN_SCREEN_ID);
	if (!screen.IsValid())
		return B_ERROR;

	if (value != NULL) {
		float wanted = strtof(value, NULL);
		if (wanted < 0.0f || wanted > 1.0f) {
			fputs("Brightness runs from 0 to 1.\n", stderr);
			return B_BAD_VALUE;
		}
		status_t status = screen.SetBrightness(wanted);
		if (status != B_OK)
			return status;
	}

	float current = -1.0f;
	status_t status = screen.GetBrightness(&current);
	if (status != B_OK)
		return status;
	printf("Backlight at %d%%\n", (int)(current * 100.0f + 0.5f));

	// The same indicator the brightness keys raise, so it can be seen on a
	// keyboard whose keys never reach the system.
	BNotification notification(B_PROGRESS_NOTIFICATION);
	notification.SetMessageID("intel_gfx_brightness");
	notification.SetGroup("Screen");
	notification.SetTitle("Brightness");
	BString content;
	content.SetToFormat("%d%%", (int)(current * 100.0f + 0.5f));
	notification.SetContent(content);
	notification.SetProgress(current);
	notification.Send(1500000);
	return B_OK;
}


int main(int argc, char** argv)
{
	status_t status = B_BAD_VALUE;
	if (argc >= 2 && strcmp(argv[1], "brightness") == 0) {
		status = Brightness(argc > 2 ? argv[2] : NULL);
	} else if (argc == 2 && strcmp(argv[1], "list") == 0) {
		BMessage reply;
		status = ListDevices(reply);
		if (status == B_OK)
			status = PrintDevices(reply);
	} else if (argc == 2 && strcmp(argv[1], "service-info") == 0) {
		BMessenger server(kServerSignature);
		BMessage request(kListDevices), reply;
		status = server.SendMessage(&request, &reply, kServerTimeout, kServerTimeout);
		if (status == B_OK) {
			int32 serviceStatus;
			uint32 abi;
			if (reply.FindInt32("status", &serviceStatus) != B_OK
				|| reply.FindUInt32("abi", &abi) != B_OK || abi != kABIVersion)
				status = B_BAD_DATA;
			else if ((status = serviceStatus) == B_OK)
				status = PrintDevices(reply);
		}
	} else if (argc == 3 && (strcmp(argv[1], "info") == 0
		|| strcmp(argv[1], "buffer-test") == 0
		|| strcmp(argv[1], "gtt-test") == 0
		|| strcmp(argv[1], "submit-test") == 0
		|| strcmp(argv[1], "engine-status") == 0
		|| strcmp(argv[1], "displays") == 0
		|| strcmp(argv[1], "fill-test") == 0
		|| strcmp(argv[1], "bench") == 0
		|| strcmp(argv[1], "vblank") == 0)) {
		Device device;
		status = device.Open(argv[2]);
		if (status == B_OK) {
			if (strcmp(argv[1], "buffer-test") == 0)
				status = BufferTest(device, argv[2]);
			else if (strcmp(argv[1], "gtt-test") == 0)
				status = GttTest(device, argv[2]);
			else if (strcmp(argv[1], "submit-test") == 0)
				status = SubmitTest(device);
			else if (strcmp(argv[1], "displays") == 0)
				status = Displays(device);
			else if (strcmp(argv[1], "fill-test") == 0)
				status = FillTest(device);
			else if (strcmp(argv[1], "bench") == 0)
				status = Bench(device);
			else if (strcmp(argv[1], "vblank") == 0)
				status = Vblank(device);
			else if (strcmp(argv[1], "engine-status") == 0) {
				EngineStatus engine;
				status = device.Status(engine);
				if (status == B_OK)
					PrintEngineStatus(engine);
			}
			else {
				DeviceInfo info;
				status = device.GetInfo(info);
				if (status == B_OK)
					PrintInfo(info);
			}
		}
	} else {
		fprintf(stderr, "Usage: %s list | service-info | info DEVICE"
			" | buffer-test DEVICE | gtt-test DEVICE"
			" | submit-test DEVICE | engine-status DEVICE"
			" | displays DEVICE | fill-test DEVICE | vblank DEVICE"
			" | brightness [0..1]\n", argv[0]);
		return 2;
	}
	if (status != B_OK)
		fprintf(stderr, "IntelGfx: %s (%" B_PRId32 ")\n", strerror(status), status);
	return status == B_OK ? 0 : 1;
}
