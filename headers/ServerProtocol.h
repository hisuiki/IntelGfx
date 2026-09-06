/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_SERVER_PROTOCOL_H
#define INTEL_GFX_SERVER_PROTOCOL_H

#include <SupportDefs.h>

namespace IntelGfx {
static const char kServerSignature[] = "application/x-vnd.IntelGfx";
static const uint32 kListDevices = 'igls';
static const bigtime_t kServerTimeout = 5000000;
}
#endif
