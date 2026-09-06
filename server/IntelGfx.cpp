/* SPDX-License-Identifier: MIT */
#include "DeviceRoster.h"
#include "IntelGfxABI.h"
#include "ServerProtocol.h"

#include <Application.h>
#include <Message.h>

class IntelGfxServer : public BApplication {
public:
	IntelGfxServer() : BApplication(IntelGfx::kServerSignature) {}
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
