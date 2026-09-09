/* SPDX-License-Identifier: MIT */
#include "DeviceRoster.h"
#include "IntelGfxABI.h"
#include "ServerProtocol.h"

#include <Application.h>
#include <Message.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* kRenderer = "/boot/system/add-ons/opengl/Intel Gallium";
static const char* kRendererRegistration
	= "/boot/system/non-packaged/add-ons/opengl/Intel Gallium";


static bool
is_renderer_registration()
{
	char target[PATH_MAX];
	ssize_t length = readlink(kRendererRegistration, target, sizeof(target) - 1);
	if (length < 0)
		return false;
	target[length] = '\0';
	return strcmp(target, kRenderer) == 0;
}

class IntelGfxServer : public BApplication {
public:
	IntelGfxServer()
		:
		BApplication(IntelGfx::kServerSignature),
		fRegisteredRenderer(false)
	{
	}

	~IntelGfxServer() override
	{
		if (fRegisteredRenderer && is_renderer_registration())
			unlink(kRendererRegistration);
	}

	void ReadyToRun() override
	{
		// Haiku checks non-packaged OpenGL add-ons before package add-ons. The
		// binary remains owned by the removable package; this service owns only
		// its priority registration and removes it when the service is stopped.
		mkdir("/boot/system/non-packaged/add-ons", 0755);
		mkdir("/boot/system/non-packaged/add-ons/opengl", 0755);
		if (is_renderer_registration()) {
			fRegisteredRenderer = true;
		} else if (symlink(kRenderer, kRendererRegistration) == 0) {
			fRegisteredRenderer = true;
		} else {
			fprintf(stderr, "IntelGfx: renderer registration failed: %s\n",
				strerror(errno));
		}

		// Enumerating /dev/graphics makes device_manager load and publish the
		// packaged kernel driver at boot. Rendering clients still open their
		// own isolated device handles; this is automatic discovery only.
		BMessage devices;
		status_t status = IntelGfx::ListDevices(devices);
		if (status != B_OK) {
			fprintf(stderr, "IntelGfx: automatic device discovery failed: %s\n",
				strerror(status));
			return;
		}

		BMessage device;
		if (devices.FindMessage("device", 0, &device) != B_OK)
			fprintf(stderr, "IntelGfx: no supported render device found\n");
	}

	void MessageReceived(BMessage* message) override
	{
		if (message->what != IntelGfx::kListDevices) {
			BApplication::MessageReceived(message);
			return;
		}
		BMessage reply(B_REPLY);
		reply.AddUInt32("abi", IntelGfx::kABIVersion);
		reply.AddInt32("status", IntelGfx::ListDevices(reply));
		message->SendReply(&reply);
	}

private:
	bool fRegisteredRenderer;
};

int main()
{
	IntelGfxServer application;
	application.Run();
	return 0;
}
