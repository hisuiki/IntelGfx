/* SPDX-License-Identifier: MIT */
// Mesa 22 Iris compatibility adapter. Linux-shaped records stay in process;
// every device operation uses the versioned IntelGfx ABI and Haiku areas.
#include "IntelGfxABI.h"
#include "intel_haiku.h"
#include "xf86drm.h"
#include "drm-uapi/i915_drm.h"
#include <OS.h>
#include <errno.h>
#include <inttypes.h>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

using namespace IntelGfx;
namespace {
struct Buffer { area_id area; uint64_t size, address; uint32_t tiling; };
struct Sync { int fd; bool signaled; };
std::mutex lock;
std::condition_variable changed;
std::map<std::pair<int, uint32_t>, Buffer> buffers;
std::map<uint32_t, Sync> syncs;
uint32_t nextSync = 1;
int fail(int error) { errno = error; return -1; }
// Iris responds to a failed submission by signalling a batch syncobj it never
// created, so the process dies far from the call that failed. Name the failing
// step while its errno is still the one that caused it.
int submitFailed(const char* step, uint32_t handle)
{
 fprintf(stderr, "IntelGfx: %s failed for buffer %" PRIu32 ": %s\n",
  step, handle, strerror(errno));
 return -1;
}

template<class T> int call(int fd, uint32_t op, T& request)
{
 return ioctl(fd, op, &request, sizeof(request));
}
Buffer* buffer(int fd, uint32_t handle)
{
 auto it = buffers.find({fd, handle});
 return it == buffers.end() ? NULL : &it->second;
}
int reg(int fd, uint32_t offset, uint32_t& value)
{
 auto r = Request<ReadRegister>(); r.offset = offset;
 if (call(fd, kReadRegister, r) < 0) return -1;
 value = r.value; return 0;
}
int topology(int fd, drm_i915_query_item& item)
{
 const size_t size = sizeof(drm_i915_query_topology_info) + 1 + 3 + 12;
 if (item.length == 0) { item.length = size; return 0; }
 if (item.length < (int)size || !item.data_ptr) return fail(EINVAL);
 // Ask the driver rather than reading the fuse registers here. They are in a
 // forcewake domain and answer zero while the GPU is powered down, which is
 // where a process that has not drawn yet finds it; the driver read them once
 // while it held the domain awake.
 auto topology = Request<IntelGfx::Topology>();
 if (call(fd, kGetTopology, topology) < 0) return -1;
 auto t = (drm_i915_query_topology_info*)(uintptr_t)item.data_ptr;
 memset(t, 0, size);
 t->max_slices = topology.maxSlices;
 t->max_subslices = topology.maxSubslices;
 t->max_eus_per_subslice = topology.maxEusPerSubslice;
 t->subslice_offset = 1; t->subslice_stride = 1;
 t->eu_offset = 4; t->eu_stride = 1;
 t->data[0] = topology.sliceMask;
 for (unsigned s = 0; s < 3; s++) {
  if (!(topology.sliceMask & (1 << s))) continue;
  t->data[1 + s] = topology.subsliceMask[s];
  for (unsigned ss = 0; ss < 4; ss++)
   t->data[4 + s * 4 + ss] = (topology.euMask[s] >> (ss * 8)) & 0xff;
 }
 if (t->data[0] == 0) {
  fprintf(stderr, "IntelGfx: the driver reports no enabled shader slice\n");
  return fail(ENODEV);
 }
 return 0;
}
int wait_syncs(int fd, drm_syncobj_wait& r, std::unique_lock<std::mutex>& guard)
{
 if (!r.count_handles || !r.handles) return fail(EINVAL);
 auto handles = (const uint32_t*)(uintptr_t)r.handles;
 for (;;) {
  bool all = true;
  for (uint32_t i = 0; i < r.count_handles; i++) {
   auto it = syncs.find(handles[i]);
   if (it == syncs.end() || it->second.fd != fd) return fail(EINVAL);
   if (it->second.signaled && !(r.flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)) {
    r.first_signaled = i; return 0;
   }
   all &= it->second.signaled;
  }
  if (all) return 0;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
  if (r.timeout_nsec <= now) return fail(ETIME);
  // Recheck absolute CLOCK_MONOTONIC time after every wake, including
  // spurious wakeups; no fabricated GPU completion on timeout.
  int64_t remaining = r.timeout_nsec - now;
  changed.wait_for(guard, std::chrono::nanoseconds(remaining));
 }
}
}

extern "C" int
intel_haiku_ioctl(int fd, unsigned long op, void* data)
{
 std::unique_lock<std::mutex> guard(lock);
 if (!data) return fail(EINVAL);
 switch (op) {
 case DRM_IOCTL_I915_GETPARAM: {
  auto& r = *(drm_i915_getparam*)data;
  auto info = Request<DeviceInfo>();
  if (!r.value || call(fd, kGetInfo, info) < 0) return -1;
  switch (r.param) {
  case I915_PARAM_CHIPSET_ID: *r.value = info.device; return 0;
  case I915_PARAM_REVISION: *r.value = info.revision; return 0;
  case I915_PARAM_HAS_LLC: *r.value = 1; return 0;
  case I915_PARAM_HAS_CONTEXT_ISOLATION:
  case I915_PARAM_HAS_EXEC_NO_RELOC:
  case I915_PARAM_HAS_EXEC_HANDLE_LUT:
  case I915_PARAM_HAS_EXEC_BATCH_FIRST:
  case I915_PARAM_HAS_EXEC_FENCE_ARRAY:
   *r.value = (info.capabilities & kNativeRender) != 0; return 0;
  case I915_PARAM_CS_TIMESTAMP_FREQUENCY: *r.value = 12000000; return 0;
  case I915_PARAM_MMAP_GTT_VERSION:
  case I915_PARAM_HAS_USERPTR_PROBE: *r.value = 0; return 0;
  default: return fail(ENOTSUP);
  }
 }
 case DRM_IOCTL_I915_GEM_CREATE: {
  auto& r = *(drm_i915_gem_create*)data;
  auto create = Request<CreateBuffer>(); create.size = r.size;
  if (call(fd, kCreateBuffer, create) < 0) return -1;
  buffers[{fd, create.handle}] = {create.area, create.size, 0, 0};
  r.handle = create.handle; r.size = create.size; return 0;
 }
 case DRM_IOCTL_GEM_CLOSE: {
  auto& r = *(drm_gem_close*)data;
  auto close = Request<CloseBuffer>(); close.handle = r.handle;
  if (call(fd, kCloseBuffer, close) < 0) return -1;
  buffers.erase({fd, r.handle}); return 0;
 }
 case DRM_IOCTL_I915_GEM_MMAP: {
  auto& r = *(drm_i915_gem_mmap*)data;
  Buffer* bo = buffer(fd, r.handle);
  if (!bo || r.offset || r.size != bo->size) return fail(EINVAL);
  void* address = NULL;
  area_id area = clone_area("Iris buffer", &address, B_ANY_ADDRESS,
   B_READ_AREA | B_WRITE_AREA, bo->area);
  if (area < 0) return fail(B_TO_POSIX_ERROR(area));
  r.addr_ptr = (uintptr_t)address; return 0;
 }
 case DRM_IOCTL_I915_GEM_BUSY: {
  auto& r = *(drm_i915_gem_busy*)data;
  if (!buffer(fd, r.handle)) return fail(EINVAL);
  r.busy = 0; return 0; // Successful submissions have completed before returning.
 }
 case DRM_IOCTL_I915_GEM_WAIT: {
  auto& r = *(drm_i915_gem_wait*)data;
  return buffer(fd, r.bo_handle) ? 0 : fail(EINVAL);
 }
 case DRM_IOCTL_I915_GEM_MADVISE: {
  auto& r = *(drm_i915_gem_madvise*)data;
  if (!buffer(fd, r.handle)) return fail(EINVAL);
  r.retained = 1; return 0; // Locked areas are retained, never purged.
 }
 case DRM_IOCTL_I915_GEM_SET_DOMAIN: {
  auto& r = *(drm_i915_gem_set_domain*)data;
  Buffer* bo = buffer(fd, r.handle);
  if (!bo) return fail(EINVAL);
  auto cache = Request<CacheBuffer>(); cache.handle = r.handle;
  cache.length = bo->size; return call(fd, kCacheBuffer, cache);
 }
 case DRM_IOCTL_I915_GEM_SET_CACHING: {
  auto& r = *(drm_i915_gem_caching*)data;
  return buffer(fd, r.handle) && r.caching == 1 ? 0 : fail(ENOTSUP);
 }
 case DRM_IOCTL_I915_GEM_SET_TILING: {
  auto& r = *(drm_i915_gem_set_tiling*)data;
  Buffer* bo = buffer(fd, r.handle);
  if (!bo) return fail(EINVAL);
  auto layout = Request<BufferLayout>(); layout.handle = r.handle;
  layout.tiling = r.tiling_mode; layout.stride = r.stride;
  if (call(fd, kBufferLayout, layout) < 0) return -1;
  bo->tiling = r.tiling_mode; r.swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
  return 0;
 }
 case DRM_IOCTL_I915_GEM_GET_TILING: {
  auto& r = *(drm_i915_gem_get_tiling*)data;
  Buffer* bo = buffer(fd, r.handle);
  if (!bo) return fail(EINVAL);
  r.tiling_mode = bo->tiling;
  r.swizzle_mode = r.phys_swizzle_mode = I915_BIT_6_SWIZZLE_NONE; return 0;
 }
 case DRM_IOCTL_I915_GEM_GET_APERTURE: {
  auto& r = *(drm_i915_gem_get_aperture*)data;
  r.aper_size = r.aper_available_size = kClientMemoryLimit; return 0;
 }
 case DRM_IOCTL_I915_GEM_CONTEXT_CREATE: {
  auto& r = *(drm_i915_gem_context_create*)data;
  auto context = Request<CreateContext>();
  if (call(fd, kCreateContext, context) < 0) return -1;
  r.ctx_id = context.context; return 0;
 }
 case DRM_IOCTL_I915_GEM_CONTEXT_DESTROY: {
  auto& r = *(drm_i915_gem_context_destroy*)data;
  auto context = Request<DestroyContext>(); context.context = r.ctx_id;
  return call(fd, kDestroyContext, context);
 }
 case DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM: {
  auto& r = *(drm_i915_gem_context_param*)data;
  if (r.param == I915_CONTEXT_PARAM_GTT_SIZE) { r.value = kVirtualAddressLimit; return 0; }
  return fail(ENOTSUP);
 }
 case DRM_IOCTL_I915_QUERY: {
  auto& r = *(drm_i915_query*)data;
  auto items = (drm_i915_query_item*)(uintptr_t)r.items_ptr;
  if (!items || r.num_items != 1) return fail(EINVAL);
  if (items[0].query_id == DRM_I915_QUERY_TOPOLOGY_INFO)
   return topology(fd, items[0]);
  return fail(ENOTSUP); // Selects separate legacy-style logical contexts in Iris.
 }
 case DRM_IOCTL_I915_REG_READ: {
  auto& r = *(drm_i915_reg_read*)data;
  // Timestamp is the only 64-bit register read Iris needs. Read high/low/high
  // so a wrap between reads cannot report time moving backwards.
  if ((r.offset & ~1ULL) != 0x2358) return fail(ENOTSUP);
  uint32_t high, low, check;
  do {
   if (reg(fd, 0x235c, high) < 0 || reg(fd, 0x2358, low) < 0
    || reg(fd, 0x235c, check) < 0) return -1;
  } while (high != check);
  r.val = ((uint64_t)high << 32) | low; return 0;
 }
 case DRM_IOCTL_SYNCOBJ_CREATE: {
  auto& r = *(drm_syncobj_create*)data;
  if (!nextSync) return fail(ENOMEM);
  r.handle = nextSync++;
  syncs[r.handle] = {fd, (r.flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0}; return 0;
 }
 case DRM_IOCTL_SYNCOBJ_DESTROY: {
  auto& r = *(drm_syncobj_destroy*)data;
  auto it = syncs.find(r.handle);
  if (it == syncs.end() || it->second.fd != fd) return fail(EINVAL);
  syncs.erase(it); changed.notify_all(); return 0;
 }
 case DRM_IOCTL_SYNCOBJ_SIGNAL:
 case DRM_IOCTL_SYNCOBJ_RESET: {
  auto& r = *(drm_syncobj_array*)data;
  auto handles = (uint32_t*)(uintptr_t)r.handles;
  if (!handles || !r.count_handles) return fail(EINVAL);
  for (uint32_t i = 0; i < r.count_handles; i++) {
   auto it = syncs.find(handles[i]);
   if (it == syncs.end() || it->second.fd != fd) return fail(EINVAL);
  }
  for (uint32_t i = 0; i < r.count_handles; i++)
   syncs[handles[i]].signaled = op == DRM_IOCTL_SYNCOBJ_SIGNAL;
  changed.notify_all(); return 0;
 }
 case DRM_IOCTL_SYNCOBJ_WAIT:
  return wait_syncs(fd, *(drm_syncobj_wait*)data, guard);
 case DRM_IOCTL_I915_GEM_EXECBUFFER2: {
  auto& r = *(drm_i915_gem_execbuffer2*)data;
  if (!r.buffer_count || r.buffer_count > kMaxBuffers || !r.buffers_ptr
   || !(r.flags & I915_EXEC_BATCH_FIRST)
   || (r.flags & I915_EXEC_RING_MASK) != I915_EXEC_RENDER)
   return fail(EINVAL);
  auto objects = (drm_i915_gem_exec_object2*)(uintptr_t)r.buffers_ptr;
  auto fences = (drm_i915_gem_exec_fence*)(uintptr_t)r.cliprects_ptr;
  if (r.flags & I915_EXEC_FENCE_ARRAY) {
   if (r.num_cliprects && !fences) return fail(EINVAL);
   for (uint32_t i = 0; i < r.num_cliprects; i++) {
    auto it = syncs.find(fences[i].handle);
    if (it == syncs.end() || it->second.fd != fd) return fail(EINVAL);
    if (fences[i].flags & I915_EXEC_FENCE_WAIT) {
     uint32_t handle = fences[i].handle;
     drm_syncobj_wait wait = {};
     wait.handles = (uintptr_t)&handle; wait.count_handles = 1;
     wait.timeout_nsec = INT64_MAX;
     if (wait_syncs(fd, wait, guard) < 0) return -1;
    }
   }
  }
  Buffer* batch = buffer(fd, objects[0].handle);
  if (!batch || r.batch_start_offset >= batch->size
   || r.batch_len > batch->size - r.batch_start_offset)
   return fail(EINVAL);
  // A zero length means the rest of the batch object, which is what i915
  // documents and what Iris relies on whenever it chained to a second batch
  // buffer and so never recorded a size for the first. The commands still end
  // at their MI_BATCH_BUFFER_END; the length only bounds what may be fetched.
  uint64_t length = r.batch_len != 0 ? r.batch_len
   : batch->size - r.batch_start_offset;

  auto submit = Request<SubmitObjects>();
  submit.context = r.rsvd1; submit.batchHandle = objects[0].handle;
  submit.offset = r.batch_start_offset; submit.length = length;
  submit.count = r.buffer_count;
  for (uint32_t i = 0; i < r.buffer_count; i++) {
   Buffer* bo = buffer(fd, objects[i].handle);
   if (!bo || objects[i].relocation_count) return fail(EINVAL);
   uint64_t address = objects[i].offset;
   if (bo->address && bo->address != address) {
    auto unbind = Request<UnbindBuffer>(); unbind.handle = objects[i].handle;
    if (call(fd, kUnbindVirtual, unbind) < 0)
     return submitFailed("unbind", objects[i].handle);
    bo->address = 0;
   }
   auto bind = Request<BindVirtual>(); bind.handle = objects[i].handle;
   bind.address = address;
   if (call(fd, kBindVirtual, bind) < 0)
    return submitFailed("bind", objects[i].handle);
   bo->address = address; submit.handles[i] = objects[i].handle;
  }
  // Keep the adapter lock through the synchronous ioctl: BO destruction,
  // fd reuse and CPU fence signaling cannot race GPU completion.
  if (call(fd, kSubmitObjects, submit) < 0)
   return submitFailed("submit", submit.batchHandle);
  if (r.flags & I915_EXEC_FENCE_ARRAY) {
   for (uint32_t i = 0; i < r.num_cliprects; i++)
    if (fences[i].flags & I915_EXEC_FENCE_SIGNAL)
     syncs[fences[i].handle].signaled = true;
   changed.notify_all();
  }
  return 0;
 }
 default: return fail(ENOTSUP);
 }
}

extern "C" int
intel_haiku_unmap(void* address, size_t size)
{
 if (!address || !size) return 0;
 area_id area = area_for(address);
 area_info info;
 if (area < 0 || get_area_info(area, &info) != B_OK
  || info.address != address || info.size != size) return fail(EINVAL);
 status_t result = delete_area(area);
 return result == B_OK ? 0 : fail(B_TO_POSIX_ERROR(result));
}

extern "C" int
drmGetDevice2(int fd, uint32_t, drmDevicePtr* device)
{
 auto info = Request<DeviceInfo>();
 if (!device || call(fd, kGetInfo, info) < 0) return -1;
 if (info.header.version != kABIVersion || !(info.capabilities & kNativeRender))
  return fail(ENODEV);
 drmDevicePtr d = (drmDevicePtr)calloc(1, sizeof(drmDevice));
 if (!d) return fail(ENOMEM);
 d->businfo.pci = (_drmPciBusInfo*)calloc(1, sizeof(_drmPciBusInfo));
 d->deviceinfo.pci = (_drmPciDeviceInfo*)calloc(1, sizeof(_drmPciDeviceInfo));
 if (!d->businfo.pci || !d->deviceinfo.pci) { drmFreeDevice(&d); return fail(ENOMEM); }
 d->businfo.pci->bus = info.bus; d->businfo.pci->dev = info.slot;
 d->businfo.pci->func = info.function;
 d->deviceinfo.pci->vendor_id = info.vendor;
 d->deviceinfo.pci->device_id = info.device;
 d->deviceinfo.pci->revision_id = info.revision;
 *device = d; return 0;
}
extern "C" void drmFreeDevice(drmDevicePtr* device)
{
 if (device && *device) {
  free((*device)->businfo.pci); free((*device)->deviceinfo.pci);
  free(*device); *device = NULL;
 }
}
extern "C" int drmPrimeFDToHandle(int, int, uint32_t*) { return fail(ENOTSUP); }
extern "C" int drmPrimeHandleToFD(int, uint32_t, uint32_t, int*) { return fail(ENOTSUP); }
