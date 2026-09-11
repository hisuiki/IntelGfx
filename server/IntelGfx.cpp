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

struct Registration {
	const char*	target;
	const char*	link;
	const char*	directory;
};

// Haiku searches the non-packaged tiers before the packaged ones, so a symlink
// there is how a package can take priority over one already installed.
//
// The renderer needs this because Haiku's OpenGL roster would otherwise pick
// Mesa's Software Pipe first. libEGL and libGLESv2 need it because the stock
// mesa package owns those names and its Haiku EGL driver has never worked --
// its Initialize hook is a hardcoded `return EGL_FALSE`. Shadowing them is safe
// on hardware without an IntelGfx device too: this EGL then fails to
// initialize, which is exactly what the stock one does anyway.
//
// Every binary stays owned by the removable package; the service owns only
// these registrations and drops them when it stops.
static const Registration kRegistrations[] = {
	{ "/boot/system/add-ons/opengl/Intel Gallium",
	  "/boot/system/non-packaged/add-ons/opengl/Intel Gallium",
	  "/boot/system/non-packaged/add-ons/opengl" },
	{ "/boot/system/lib/intelgfx/libEGL.so.1.0.0",
	  "/boot/system/non-packaged/lib/libEGL.so.1.0.0",
	  "/boot/system/non-packaged/lib" },
	{ "libEGL.so.1.0.0", "/boot/system/non-packaged/lib/libEGL.so.1", NULL },
	{ "libEGL.so.1.0.0", "/boot/system/non-packaged/lib/libEGL.so", NULL },
	{ "/boot/system/lib/intelgfx/libGLESv2.so.2.0.0",
	  "/boot/system/non-packaged/lib/libGLESv2.so.2.0.0",
	  "/boot/system/non-packaged/lib" },
	{ "libGLESv2.so.2.0.0", "/boot/system/non-packaged/lib/libGLESv2.so.2",
	  NULL },
	{ "libGLESv2.so.2.0.0", "/boot/system/non-packaged/lib/libGLESv2.so", NULL },
};


static bool
is_our_registration(const Registration& registration)
{
	char target[PATH_MAX];
	ssize_t length = readlink(registration.link, target, sizeof(target) - 1);
	if (length < 0)
		return false;
	target[length] = '\0';
	return strcmp(target, registration.target) == 0;
}

class IntelGfxServer : public BApplication {
public:
	IntelGfxServer()
		:
		BApplication(IntelGfx::kServerSignature)
	{
	}

	~IntelGfxServer() override
	{
		for (size_t i = 0; i < B_COUNT_OF(kRegistrations); i++) {
			if (fRegistered[i] && is_our_registration(kRegistrations[i]))
				unlink(kRegistrations[i].link);
		}
	}

	void ReadyToRun() override
	{
		mkdir("/boot/system/non-packaged", 0755);
		mkdir("/boot/system/non-packaged/add-ons", 0755);
		for (size_t i = 0; i < B_COUNT_OF(kRegistrations); i++) {
			const Registration& registration = kRegistrations[i];
			if (registration.directory != NULL)
				mkdir(registration.directory, 0755);

			if (is_our_registration(registration)) {
				fRegistered[i] = true;
			} else if (symlink(registration.target, registration.link) == 0) {
				fRegistered[i] = true;
			} else {
				fprintf(stderr, "IntelGfx: could not register %s: %s\n",
					registration.link, strerror(errno));
			}
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
	bool fRegistered[B_COUNT_OF(kRegistrations)] = {};
};

int main()
{
	IntelGfxServer application;
	application.Run();
	return 0;
}
