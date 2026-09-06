/* SPDX-License-Identifier: MIT */
/* Iris's internal libdrm vocabulary. No DRM requests cross the Haiku ABI. */
#ifndef INTEL_HAIKU_DRM_COMPAT_H
#define INTEL_HAIKU_DRM_COMPAT_H
#include <stdint.h>
#include "drm-uapi/drm.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRM_BUS_PCI 0
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER 2
#define DRM_NODE_MAX 3
#define DRM_DEVICE_GET_PCI_REVISION 1
struct _drmPciBusInfo { uint16_t domain; uint8_t bus, dev, func; };
struct _drmPciDeviceInfo { uint16_t vendor_id, device_id, subvendor_id, subdevice_id; uint8_t revision_id; };
typedef struct _drmDevice {
 char **nodes; int available_nodes; int bustype;
 union { struct _drmPciBusInfo *pci; } businfo;
 union { struct _drmPciDeviceInfo *pci; } deviceinfo;
} drmDevice, *drmDevicePtr;
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int intel_haiku_ioctl(int fd, unsigned long request, void *arg);
#define drmIoctl intel_haiku_ioctl
#ifdef __cplusplus
}
#endif
#endif
