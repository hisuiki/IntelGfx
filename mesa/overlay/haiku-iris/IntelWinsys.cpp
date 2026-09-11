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
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

using namespace IntelGfx;
namespace {
struct Buffer {
 area_id area; uint64_t size, address; uint32_t tiling;
 // Iris does not close a buffer it has finished with: it keeps the handle in
 // its own cache, marks it purgeable with madvise(DONTNEED) and asks for it
 // back later with WILLNEED. On i915 the kernel is free to drop a purgeable
 // buffer's pages, and the WILLNEED that finds it gone answers retained = 0,
 // which is how Iris learns to allocate a fresh one instead. Keep the same
 // state here, because the driver charges every cached handle against this
 // client's buffer-slot and memory limits: with nothing ever purged the cache
 // only grows, and the allocation that eventually fails makes WebRender fall
 // back to software rendering for the rest of the session.
 bool purgeable, purged;
 uint64_t stamp;
};
struct Sync { int fd; bool signaled; };
// Orders purgeable buffers so reclaim starts with the ones Iris has left alone
// the longest.
uint64_t purgeClock = 0;
// Reclaim several buffers at once rather than one per failed allocation, which
// would leave us sitting at the limit paying a failed ioctl for every alloc.
static const uint32_t kReclaimBatch = 16;
// How many buffers the cache may hold before it is trimmed anyway. The client's
// whole table is kMaxBuffers entries and the live compositor needs most of
// them, so the cache does not get to keep a large share of it.
static const uint32_t kMaxCached = 64;
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
// Hand the driver back the buffers Iris has marked purgeable, oldest first,
// until at least `bytes` and `count` buffers' worth of the client's limits are
// free again. The map entry stays behind marked purged: Iris reads retained
// out of its own request structure, which it initialises to 1, so a madvise
// that failed outright would read as "still there" and it would go on using a
// buffer that no longer exists. Only an explicit retained = 0 makes it let go.
uint32_t reclaim(int fd, uint64_t bytes, uint32_t count)
{
 uint64_t freed = 0;
 uint32_t purged = 0;
 while (purged < count || freed < bytes) {
  Buffer* oldest = NULL;
  uint32_t handle = 0;
  for (auto& entry : buffers) {
   if (entry.first.first != fd) continue;
   Buffer& bo = entry.second;
   if (!bo.purgeable || bo.purged) continue;
   if (oldest == NULL || bo.stamp < oldest->stamp) {
    oldest = &bo; handle = entry.first.second;
   }
  }
  if (oldest == NULL) break;
  auto close = Request<CloseBuffer>(); close.handle = handle;
  if (call(fd, kCloseBuffer, close) < 0) break;
  oldest->purged = true;
  oldest->address = 0;
  oldest->area = -1;
  freed += oldest->size;
  purged++;
 }
 return purged;
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
  case I915_PARAM_HAS_WAIT_TIMEOUT:
  case I915_PARAM_HAS_EXECBUF2:
  case I915_PARAM_HAS_EXEC_SOFTPIN:
   *r.value = (info.capabilities & kNativeRender) != 0; return 0;
  case I915_PARAM_CS_TIMESTAMP_FREQUENCY: *r.value = 12000000; return 0;
  case I915_PARAM_MMAP_GTT_VERSION:
  case I915_PARAM_HAS_USERPTR_PROBE:
  case I915_PARAM_HAS_EXEC_ASYNC:
  case I915_PARAM_HAS_EXEC_CAPTURE:
  case I915_PARAM_HAS_EXEC_TIMELINE_FENCES:
  case I915_PARAM_CMD_PARSER_VERSION:
  case I915_PARAM_MMAP_VERSION:
   *r.value = 0; return 0;
  default: return fail(ENOTSUP);
  }
 }
 case DRM_IOCTL_I915_GEM_CREATE: {
  auto& r = *(drm_i915_gem_create*)data;
  auto create = Request<CreateBuffer>(); create.size = r.size;
  if (call(fd, kCreateBuffer, create) < 0) {
   // Everything Iris has cached is still charged to this client, so a
   // refusal here usually means the cache has taken the whole allowance
   // rather than that the device is out of memory. Give some of it back
   // and ask again: Iris answers a failed allocation by disabling
   // hardware rendering for the rest of the session, which is far more
   // expensive than the reclaim.
   uint32_t live = 0, cached = 0;
   uint64_t bytes = 0;
   for (auto& entry : buffers) {
    if (entry.first.first != fd || entry.second.purged) continue;
    live++; bytes += entry.second.size;
    if (entry.second.purgeable) cached++;
   }
   uint32_t freed = reclaim(fd, r.size, kReclaimBatch);
   // Both limits the driver enforces answer with the same error, so say
   // which one the client was actually up against.
   fprintf(stderr, "IntelGfx: allocation of %" PRIu64 " bytes refused with "
    "%" PRIu32 " buffers live (%" PRIu32 " cached) using %" PRIu64
    " bytes; reclaimed %" PRIu32 "\n",
    (uint64_t)r.size, live, cached, bytes, freed);
   if (freed == 0) return -1;
   create = Request<CreateBuffer>(); create.size = r.size;
   if (call(fd, kCreateBuffer, create) < 0) return -1;
  }
  buffers[{fd, create.handle}] = {create.area, create.size, 0, 0, false, false, 0};
  r.handle = create.handle; r.size = create.size; return 0;
 }
 case DRM_IOCTL_GEM_CLOSE: {
  auto& r = *(drm_gem_close*)data;
  Buffer* bo = buffer(fd, r.handle);
  if (!bo) return fail(EINVAL);
  // A reclaimed buffer is already gone from the driver; all that is left is
  // the record kept so madvise could report it purged.
  if (!bo->purged) {
   auto close = Request<CloseBuffer>(); close.handle = r.handle;
   if (call(fd, kCloseBuffer, close) < 0) return -1;
  }
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
  Buffer* bo = buffer(fd, r.handle);
  if (!bo) return fail(EINVAL);
  if (bo->purged) { r.retained = 0; return 0; }
  bo->purgeable = r.madv == I915_MADV_DONTNEED;
  if (!bo->purgeable) { r.retained = 1; return 0; }
  bo->stamp = ++purgeClock;
  // Trim the cache on the way past the watermark instead of waiting for an
  // allocation to fail, so the limit is approached gently and the buffers
  // still in use keep their slots.
  uint32_t cached = 0;
  for (auto& entry : buffers) {
   if (entry.first.first == fd && entry.second.purgeable
    && !entry.second.purged)
    cached++;
  }
  if (cached > kMaxCached) reclaim(fd, 0, cached - kMaxCached);
  r.retained = bo->purged ? 0 : 1; return 0;
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
 case DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM:
  return 0;
 case DRM_IOCTL_I915_GET_RESET_STATS: {
  auto& s = *(drm_i915_reset_stats*)data;
  s.reset_count = s.batch_active = s.batch_pending = 0;
  return 0;
 }
 case DRM_IOCTL_I915_GEM_MMAP_OFFSET:
 case DRM_IOCTL_I915_GEM_CREATE_EXT:
 case DRM_IOCTL_I915_GEM_USERPTR:
  return fail(ENOTSUP);
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
 case DRM_IOCTL_I915_GEM_EXECBUFFER2_WR:
 case DRM_IOCTL_I915_GEM_EXECBUFFER2: {
  auto& r = *(drm_i915_gem_execbuffer2*)data;
  if (!r.buffer_count || r.buffer_count > kMaxBuffers || !r.buffers_ptr
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
  uint32_t batchIndex = (r.flags & I915_EXEC_BATCH_FIRST)
   ? 0 : r.buffer_count - 1;
  Buffer* batch = buffer(fd, objects[batchIndex].handle);
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
  submit.context = r.rsvd1; submit.batchHandle = objects[batchIndex].handle;
  submit.offset = r.batch_start_offset; submit.length = length;
  submit.count = r.buffer_count;
  // Iris reassigns virtual addresses freely between submissions, so a buffer's
  // new home is often still occupied by a different buffer bound during an
  // earlier one. The kernel refuses an overlapping bind with B_BUSY, and as
  // noted above Iris answers a failed submission by signalling a batch syncobj
  // it never created, so the process aborts far from here. Only the buffer
  // whose own address changed used to be unbound, which leaves exactly those
  // stale neighbours in place; evict every binding that overlaps the layout
  // this submission asks for before binding any of it.
  for (uint32_t i = 0; i < r.buffer_count; i++) {
   Buffer* bo = buffer(fd, objects[i].handle);
   if (!bo) return fail(EINVAL);
   uint64_t start = objects[i].offset, end = start + bo->size;
   // A buffer that is staying where it already is cannot have acquired a new
   // neighbour: the driver refuses any bind that overlaps a live one, so the
   // range is already known to be clear. Only a buffer that is moving needs
   // the sweep, which in a steady frame is almost none of them.
   if (bo->address == start) continue;
   for (auto& entry : buffers) {
    if (entry.first.first != fd) continue;
    Buffer& other = entry.second;
    if (&other == bo || other.address == 0) continue;
    if (start < other.address + other.size && other.address < end) {
     auto evict = Request<UnbindBuffer>(); evict.handle = entry.first.second;
     if (call(fd, kUnbindVirtual, evict) < 0)
      return submitFailed("unbind", entry.first.second);
     other.address = 0;
    }
   }
  }

  for (uint32_t i = 0; i < r.buffer_count; i++) {
   Buffer* bo = buffer(fd, objects[i].handle);
   if (!bo || objects[i].relocation_count) return fail(EINVAL);
   uint64_t address = objects[i].offset;
   // Iris names every buffer a batch touches on each submission, but between
   // frames it usually leaves them where they were. Rebinding one to the
   // address it already has is a syscall the driver answers by doing nothing,
   // and there is one of them per buffer per frame, so skip it here instead.
   if (bo->address != address) {
    if (bo->address) {
     auto unbind = Request<UnbindBuffer>(); unbind.handle = objects[i].handle;
     if (call(fd, kUnbindVirtual, unbind) < 0)
      return submitFailed("unbind", objects[i].handle);
     bo->address = 0;
    }
    auto bind = Request<BindVirtual>(); bind.handle = objects[i].handle;
    bind.address = address;
    if (call(fd, kBindVirtual, bind) < 0)
     return submitFailed("bind", objects[i].handle);
    bo->address = address;
   }
   submit.handles[i] = objects[i].handle;
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
 d->nodes = (char**)calloc(DRM_NODE_MAX, sizeof(char*));
 if (!d->businfo.pci || !d->deviceinfo.pci || !d->nodes) { drmFreeDevice(&d); return fail(ENOMEM); }
 d->bustype = DRM_BUS_PCI;
 d->businfo.pci->domain = 0;
 d->businfo.pci->bus = info.bus; d->businfo.pci->dev = info.slot;
 d->businfo.pci->func = info.function;
 d->deviceinfo.pci->vendor_id = info.vendor;
 d->deviceinfo.pci->device_id = info.device;
 d->deviceinfo.pci->revision_id = info.revision;
 d->available_nodes = (1 << DRM_NODE_PRIMARY) | (1 << DRM_NODE_RENDER);
 d->nodes[DRM_NODE_PRIMARY] = strdup("/dev/graphics/intel_extreme");
 d->nodes[DRM_NODE_RENDER] = strdup("/dev/graphics/intel_extreme");
 *device = d; return 0;
}

extern "C" int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
 if (!devices || max_devices <= 0) return 0;
 DIR* dir = opendir("/dev/graphics");
 if (!dir) return 0;
 struct dirent* entry;
 int count = 0;
 while ((entry = readdir(dir)) != NULL && count < max_devices) {
  if (strncmp(entry->d_name, "intel_extreme_", 14) != 0)
   continue;
  char path[128];
  snprintf(path, sizeof(path), "/dev/graphics/%s", entry->d_name);
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) continue;
  auto info = Request<DeviceInfo>();
  if (call(fd, kGetInfo, info) < 0 || info.header.version != kABIVersion
   || !(info.capabilities & kNativeRender)) {
   close(fd);
   continue;
  }
  close(fd);

  drmDevicePtr d = (drmDevicePtr)calloc(1, sizeof(drmDevice));
  if (!d) continue;
  d->businfo.pci = (_drmPciBusInfo*)calloc(1, sizeof(_drmPciBusInfo));
  d->deviceinfo.pci = (_drmPciDeviceInfo*)calloc(1, sizeof(_drmPciDeviceInfo));
  d->nodes = (char**)calloc(DRM_NODE_MAX, sizeof(char*));
  if (!d->businfo.pci || !d->deviceinfo.pci || !d->nodes) {
   drmFreeDevice(&d);
   continue;
  }
  d->bustype = DRM_BUS_PCI;
  d->businfo.pci->domain = 0;
  d->businfo.pci->bus = info.bus;
  d->businfo.pci->dev = info.slot;
  d->businfo.pci->func = info.function;
  d->deviceinfo.pci->vendor_id = info.vendor;
  d->deviceinfo.pci->device_id = info.device;
  d->deviceinfo.pci->revision_id = info.revision;
  d->available_nodes = (1 << DRM_NODE_PRIMARY) | (1 << DRM_NODE_RENDER);
  d->nodes[DRM_NODE_PRIMARY] = strdup(path);
  d->nodes[DRM_NODE_RENDER] = strdup(path);
  devices[count++] = d;
 }
 closedir(dir);
 return count;
}

extern "C" void drmFreeDevice(drmDevicePtr* device)
{
 if (device && *device) {
  if ((*device)->nodes) {
   for (int i = 0; i < DRM_NODE_MAX; i++)
    free((*device)->nodes[i]);
   free((*device)->nodes);
  }
  free((*device)->businfo.pci);
  free((*device)->deviceinfo.pci);
  free(*device);
  *device = NULL;
 }
}

extern "C" void drmFreeDevices(drmDevicePtr devices[], int count)
{
 for (int i = 0; i < count; i++)
  drmFreeDevice(&devices[i]);
}

extern "C" int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
 if (a == b) return 1;
 if (!a || !b) return 0;
 if (a->bustype != b->bustype) return 0;
 if (a->bustype == DRM_BUS_PCI) {
  return a->businfo.pci->domain == b->businfo.pci->domain &&
         a->businfo.pci->bus == b->businfo.pci->bus &&
         a->businfo.pci->dev == b->businfo.pci->dev &&
         a->businfo.pci->func == b->businfo.pci->func;
 }
 return 0;
}

extern "C" int drmPrimeFDToHandle(int, int, uint32_t*) { return fail(ENOTSUP); }
extern "C" int drmPrimeHandleToFD(int, uint32_t, uint32_t, int*) { return fail(ENOTSUP); }

extern "C" int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
 struct drm_syncobj_create args = { .handle = 0, .flags = flags };
 int ret = intel_haiku_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args);
 if (ret == 0 && handle) *handle = args.handle;
 return ret;
}

extern "C" int drmSyncobjDestroy(int fd, uint32_t handle)
{
 struct drm_syncobj_destroy args = { .handle = handle, .pad = 0 };
 return intel_haiku_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

extern "C" int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
 struct drm_syncobj_array args = { .handles = (uintptr_t)handles, .count_handles = count, .pad = 0 };
 return intel_haiku_ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args);
}

extern "C" int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
 struct drm_syncobj_array args = { .handles = (uintptr_t)handles, .count_handles = count, .pad = 0 };
 return intel_haiku_ioctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &args);
}

extern "C" int drmSyncobjWait(int fd, uint32_t *handles, uint32_t count, int64_t timeout_nsec, uint32_t flags, uint32_t *first_signaled)
{
 struct drm_syncobj_wait args = {
  .handles = (uintptr_t)handles,
  .timeout_nsec = timeout_nsec,
  .count_handles = count,
  .flags = flags,
  .first_signaled = 0,
  .pad = 0
 };
 int ret = intel_haiku_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args);
 if (ret == 0 && first_signaled) *first_signaled = args.first_signaled;
 return ret;
}

extern "C" int drmSyncobjTimelineSignal(int, const uint32_t *, uint64_t *, uint32_t) { return -1; }
extern "C" int drmSyncobjQuery(int, uint32_t *, uint64_t *, uint32_t) { return -1; }
extern "C" int drmSyncobjTimelineWait(int, uint32_t *, uint64_t *, uint32_t, int64_t, uint32_t, uint32_t *) { return -1; }
extern "C" int drmSyncobjFDToHandle(int, int, uint32_t *) { return -1; }
extern "C" int drmSyncobjHandleToFD(int, uint32_t, int *) { return -1; }
extern "C" int drmSyncobjImportSyncFile(int, uint32_t, int) { return -1; }
extern "C" int drmSyncobjExportSyncFile(int, uint32_t, int *) { return -1; }

extern "C" int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
 if (!value) return -EINVAL;
 switch (capability) {
 case DRM_CAP_SYNCOBJ:
  *value = 1;
  return 0;
 case DRM_CAP_SYNCOBJ_TIMELINE:
  *value = 0;
  return 0;
 case DRM_CAP_PRIME:
  *value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
  return 0;
 case DRM_CAP_TIMESTAMP_MONOTONIC:
  *value = 1;
  return 0;
 default:
  *value = 0;
  return -EINVAL;
 }
}
