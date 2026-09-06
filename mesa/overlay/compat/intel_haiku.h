/* SPDX-License-Identifier: MIT */
#ifndef INTEL_HAIKU_H
#define INTEL_HAIKU_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int intel_haiku_ioctl(int fd, unsigned long request, void *arg);
int intel_haiku_unmap(void *address, size_t size);
#ifdef __cplusplus
}
#endif
#endif
