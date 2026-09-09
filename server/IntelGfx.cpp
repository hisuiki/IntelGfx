/* SPDX-License-Identifier: MIT */
#include "DeviceRoster.h"
#include "IntelGfxABI.h"
#include "ServerProtocol.h"

#include <Application.h>
#include <Message.h>
#include <stdio.h>
#include <string.h>

class IntelGfxServer : public BApplication {
public:
	IntelGfxServer() : BApplication(IntelGfx::kServerSignature) {}

	void ReadyToRun() override
	{
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
};

int main()
{
	IntelGfxServer application;
	application.Run();
	return 0;
}
