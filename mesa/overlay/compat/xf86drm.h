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
#ifndef major
#define major(dev) ((int)(((dev) >> 8) & 0xff))
#endif
#ifndef minor
#define minor(dev) ((int)((dev) & 0xff))
#endif
struct _drmPciBusInfo { uint16_t domain; uint8_t bus, dev, func; };
struct _drmPciDeviceInfo { uint16_t vendor_id, device_id, subvendor_id, subdevice_id; uint8_t revision_id; };
typedef struct _drmDevice {
 char **nodes; int available_nodes; int bustype;
 union { struct _drmPciBusInfo *pci; } businfo;
 union { struct _drmPciDeviceInfo *pci; } deviceinfo;
} drmDevice, *drmDevicePtr;
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void drmFreeDevice(drmDevicePtr *device);
void drmFreeDevices(drmDevicePtr devices[], int count);
int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int intel_haiku_ioctl(int fd, unsigned long request, void *arg);
#define drmIoctl intel_haiku_ioctl
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

/* DRM syncobj API for Vulkan synchronization */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjWait(int fd, uint32_t *handles, uint32_t count, int64_t timeout_nsec, uint32_t flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, uint32_t count, int64_t timeout_nsec, uint32_t flags, uint32_t *first_signaled);
int drmSyncobjFDToHandle(int fd, int sync_file_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
#ifdef __cplusplus
}
#endif
#endif
