/* SPDX-License-Identifier: MIT */
#include "DeviceRoster.h"
#include "Device.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace IntelGfx {

status_t ListDevices(BMessage& reply)
{
	DIR* directory = opendir("/dev/graphics");
	if (directory == NULL)
		return errno;
	status_t result = B_OK;
	while (dirent* entry = readdir(directory)) {
		if (strncmp(entry->d_name, "intel_extreme_", 14) != 0)
			continue;
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "/dev/graphics/%s", entry->d_name);
		Device device;
		status_t status = device.Open(path);
		BMessage item;
		item.AddString("path", path);
		if (status == B_OK) {
			DeviceInfo info;
			status = device.GetInfo(info);
			if (status == B_OK)
				item.AddData("info", B_RAW_TYPE, &info, sizeof(info));
		}
		item.AddInt32("status", status);
		result = reply.AddMessage("device", &item);
		if (result != B_OK)
			break;
	}
	closedir(directory);
	return result;
}

}
