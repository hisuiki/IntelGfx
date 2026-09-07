/*
 * Copyright © 2026 IntelGfx contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

struct wsi_haiku {
   struct wsi_interface base;
   struct wsi_device *wsi;
   const VkAllocationCallbacks *alloc;
   VkPhysicalDevice physical_device;
};

struct wsi_haiku_image {
   struct wsi_image base;
};

struct wsi_haiku_swapchain {
   struct wsi_swapchain base;
   struct wsi_haiku *wsi;
   VkIcdSurfaceBase *surface;
   VkResult status;
   VkExtent2D extent;
   struct wsi_haiku_image images[0];
};

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateHeadlessSurfaceEXT(VkInstance _instance,
                             const VkHeadlessSurfaceCreateInfoEXT *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   VkIcdSurfaceBase *surface = vk_zalloc2(&instance->alloc, pAllocator,
                                         sizeof(VkIcdSurfaceBase), 8,
                                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->platform = VK_ICD_WSI_PLATFORM_HEADLESS;
   *pSurface = VkIcdSurfaceBase_to_handle(surface);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateWaylandSurfaceKHR(VkInstance _instance,
                            const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   VkIcdSurfaceWayland *surface = vk_zalloc2(&instance->alloc, pAllocator,
                                            sizeof(VkIcdSurfaceWayland), 8,
                                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_WAYLAND;
   surface->display = pCreateInfo->display;
   surface->surface = pCreateInfo->surface;
   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wsi_GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                  uint32_t queueFamilyIndex,
                                                  struct wl_display *display)
{
   return VK_TRUE;
}

static VkResult
wsi_haiku_surface_get_support(VkIcdSurfaceBase *surface,
                             struct wsi_device *wsi_device,
                             uint32_t queueFamilyIndex,
                             VkBool32* pSupported)
{
   *pSupported = true;
   return VK_SUCCESS;
}

static VkResult
wsi_haiku_surface_get_capabilities(VkIcdSurfaceBase *surface,
                                  struct wsi_device *wsi_device,
                                  VkSurfaceCapabilitiesKHR* caps)
{
   caps->minImageCount = 1;
   caps->maxImageCount = 8;
   caps->currentExtent = (VkExtent2D) { UINT32_MAX, UINT32_MAX };
   caps->minImageExtent = (VkExtent2D) { 1, 1 };
   caps->maxImageExtent = (VkExtent2D) {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };
   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->maxImageArrayLayers = 1;
   caps->supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
   caps->supportedUsageFlags =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
   return VK_SUCCESS;
}

static VkResult
wsi_haiku_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                   struct wsi_device *wsi_device,
                                   const void *info_next,
                                   VkSurfaceCapabilities2KHR* caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);
   return wsi_haiku_surface_get_capabilities(surface, wsi_device,
                                             &caps->surfaceCapabilities);
}

static const struct {
   VkFormat format;
} available_surface_formats[] = {
   { .format = VK_FORMAT_B8G8R8A8_SRGB },
   { .format = VK_FORMAT_B8G8R8A8_UNORM },
   { .format = VK_FORMAT_R8G8B8A8_SRGB },
   { .format = VK_FORMAT_R8G8B8A8_UNORM },
};

static VkResult
wsi_haiku_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                             struct wsi_device *wsi_device,
                             uint32_t* pSurfaceFormatCount,
                             VkSurfaceFormatKHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = available_surface_formats[i].format;
         f->colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
wsi_haiku_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
                              struct wsi_device *wsi_device,
                              const void *info_next,
                              uint32_t* pSurfaceFormatCount,
                              VkSurfaceFormat2KHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         assert(f->sType == VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
         f->surfaceFormat.format = available_surface_formats[i].format;
         f->surfaceFormat.colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_IMMEDIATE_KHR,
};

static VkResult
wsi_haiku_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                   uint32_t* pPresentModeCount,
                                   VkPresentModeKHR* pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }
   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);
   return *pPresentModeCount < ARRAY_SIZE(present_modes) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult
wsi_haiku_surface_get_present_rectangles(VkIcdSurfaceBase *surface,
                                        struct wsi_device *wsi_device,
                                        uint32_t* pRectCount,
                                        VkRect2D* pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);
   vk_outarray_append_typed(VkRect2D, &out, rect) {
      *rect = (VkRect2D) {
         .offset = { 0, 0 },
         .extent = { UINT32_MAX, UINT32_MAX },
      };
   }
   return vk_outarray_status(&out);
}

static uint32_t
select_memory_type(const struct wsi_device *wsi,
                   VkMemoryPropertyFlags props,
                   uint32_t type_bits)
{
   for (uint32_t i = 0; i < wsi->memory_props.memoryTypeCount; i++) {
       const VkMemoryType type = wsi->memory_props.memoryTypes[i];
       if ((type_bits & (1 << i)) && (type.propertyFlags & props) == props)
         return i;
   }
   for (uint32_t i = 0; i < wsi->memory_props.memoryTypeCount; i++) {
       if (type_bits & (1 << i))
         return i;
   }
   return 0;
}

static VkResult
wsi_create_haiku_image_mem(const struct wsi_swapchain *chain,
                           const struct wsi_image_info *info,
                           struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->device, image->image, &reqs);

   const VkMemoryDedicatedAllocateInfo memory_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->image,
      .buffer = VK_NULL_HANDLE,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &memory_dedicated_info,
      .allocationSize = reqs.size,
      .memoryTypeIndex = select_memory_type(wsi, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                            reqs.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &memory_info,
                                &chain->alloc, &image->memory);
   if (result != VK_SUCCESS)
      return result;

   const VkImageSubresource image_subresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .arrayLayer = 0,
   };
   VkSubresourceLayout image_layout;
   wsi->GetImageSubresourceLayout(chain->device, image->image,
                                  &image_subresource, &image_layout);

   image->num_planes = 1;
   image->sizes[0] = reqs.size;
   image->row_pitches[0] = image_layout.rowPitch;
   image->offsets[0] = 0;
   return VK_SUCCESS;
}

static VkResult
wsi_configure_haiku_image(const struct wsi_swapchain *chain,
                          const VkSwapchainCreateInfoKHR *pCreateInfo,
                          struct wsi_image_info *info)
{
   VkResult result = wsi_configure_image(chain, pCreateInfo, 0, info);
   if (result != VK_SUCCESS)
      return result;
   info->create_mem = wsi_create_haiku_image_mem;
   return VK_SUCCESS;
}

static struct wsi_image *
wsi_haiku_get_wsi_image(struct wsi_swapchain *drv_chain,
                        uint32_t image_index)
{
   struct wsi_haiku_swapchain *chain = (struct wsi_haiku_swapchain *) drv_chain;
   return &chain->images[image_index].base;
}

static VkResult
wsi_haiku_acquire_next_image(struct wsi_swapchain *drv_chain,
                             const VkAcquireNextImageInfoKHR *info,
                             uint32_t *image_index)
{
   struct wsi_haiku_swapchain *chain = (struct wsi_haiku_swapchain *) drv_chain;
   if (chain->status != VK_SUCCESS)
      return chain->status;
   *image_index = 0;
   return VK_SUCCESS;
}

static VkResult
wsi_haiku_queue_present(struct wsi_swapchain *drv_chain,
                        uint32_t image_index,
                        const VkPresentRegionKHR *damage)
{
   struct wsi_haiku_swapchain *chain = (struct wsi_haiku_swapchain *) drv_chain;
   assert(image_index < chain->base.image_count);
   return chain->status;
}

static VkResult
wsi_haiku_swapchain_destroy(struct wsi_swapchain *drv_chain,
                            const VkAllocationCallbacks *allocator)
{
   struct wsi_haiku_swapchain *chain = (struct wsi_haiku_swapchain *) drv_chain;
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      wsi_destroy_image(&chain->base, &chain->images[i].base);
   }
   wsi_destroy_image_info(&chain->base, &chain->base.image_info);
   wsi_swapchain_finish(&chain->base);
   vk_free(allocator, chain);
   return VK_SUCCESS;
}

static VkResult
wsi_haiku_surface_create_swapchain(VkIcdSurfaceBase *icd_surface,
                                   VkDevice device,
                                   struct wsi_device *wsi_device,
                                   const VkSwapchainCreateInfoKHR *create_info,
                                   const VkAllocationCallbacks *allocator,
                                   struct wsi_swapchain **swapchain_out)
{
   struct wsi_haiku *wsi =
      (struct wsi_haiku *) wsi_device->wsi[icd_surface->platform];

   assert(create_info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
   const unsigned num_images = create_info->minImageCount;
   size_t size = sizeof(struct wsi_haiku_swapchain) + num_images * sizeof(struct wsi_haiku_image);

   struct wsi_haiku_swapchain *chain = vk_zalloc(allocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = wsi_swapchain_init(wsi_device, &chain->base, device,
                                        create_info, allocator, false);
   if (result != VK_SUCCESS) {
      vk_free(allocator, chain);
      return result;
   }

   chain->base.destroy = wsi_haiku_swapchain_destroy;
   chain->base.get_wsi_image = wsi_haiku_get_wsi_image;
   chain->base.acquire_next_image = wsi_haiku_acquire_next_image;
   chain->base.queue_present = wsi_haiku_queue_present;
   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, create_info);
   chain->base.image_count = num_images;
   chain->extent = create_info->imageExtent;
   chain->wsi = wsi;
   chain->status = VK_SUCCESS;
   chain->surface = icd_surface;

   result = wsi_configure_haiku_image(&chain->base, create_info,
                                      &chain->base.image_info);
   if (result != VK_SUCCESS) {
      vk_free(allocator, chain);
      return result;
   }

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      result = wsi_create_image(&chain->base, &chain->base.image_info,
                                &chain->images[i].base);
      if (result != VK_SUCCESS) {
         while (i > 0) {
            --i;
            wsi_destroy_image(&chain->base, &chain->images[i].base);
         }
         wsi_destroy_image_info(&chain->base, &chain->base.image_info);
         vk_free(allocator, chain);
         return result;
      }
   }

   *swapchain_out = &chain->base;
   return VK_SUCCESS;
}

VkResult
wsi_haiku_init_wsi(struct wsi_device *wsi_device,
                   const VkAllocationCallbacks *alloc,
                   VkPhysicalDevice physical_device)
{
   struct wsi_haiku *wsi = vk_alloc(alloc, sizeof(*wsi), 8,
                                    VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   wsi->physical_device = physical_device;
   wsi->alloc = alloc;
   wsi->wsi = wsi_device;

   wsi->base.get_support = wsi_haiku_surface_get_support;
   wsi->base.get_capabilities2 = wsi_haiku_surface_get_capabilities2;
   wsi->base.get_formats = wsi_haiku_surface_get_formats;
   wsi->base.get_formats2 = wsi_haiku_surface_get_formats2;
   wsi->base.get_present_modes = wsi_haiku_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_haiku_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_haiku_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_HEADLESS] = &wsi->base;
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_WAYLAND] = &wsi->base;
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] = &wsi->base;

   return VK_SUCCESS;
}

void
wsi_haiku_finish_wsi(struct wsi_device *wsi_device,
                     const VkAllocationCallbacks *alloc)
{
   struct wsi_haiku *wsi =
      (struct wsi_haiku *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_HEADLESS];
   if (!wsi)
      return;
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_HEADLESS] = NULL;
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_WAYLAND] = NULL;
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] = NULL;
   vk_free(alloc, wsi);
}
