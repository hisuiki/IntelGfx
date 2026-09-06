/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_DEVICE_ROSTER_H
#define INTEL_GFX_DEVICE_ROSTER_H

#include <Message.h>

namespace IntelGfx {
status_t ListDevices(BMessage& reply);
}
#endif
