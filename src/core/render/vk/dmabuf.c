/* render/vk dmabuf.c - zero-copy dmabuf import as a sampled VkImage */

#include <unistd.h>

#include "context.h"
#include "core/compositor.h"
#include "core/log.h"

/* Create the VkImage for the dmabuf import.
 * Builds the pNext chain for VkImageCreateInfo:
 *   (a) VkImageDrmFormatModifierExplicitCreateInfoEXT - exact modifier + plane
 *       layout, with VkSubresourceLayout.rowPitch = stride.
 *   (c) No modifier struct - explicit only.
 * VkSubresourceLayout.size = stride*h (entire image). */
static int dmabuf_create_image(uint32_t w, uint32_t h, uint32_t stride, uint32_t fourcc, uint64_t modifier,
                               VkFormat vk_fmt, VkImage *out_img) {
  vk_context_t *ctx = vk_ctx();
  VkDevice dev = ctx->device.vk_dev;
  VkResult r;
  VK_LOAD(vkCreateImage);

  VkSubresourceLayout drm_layout = {
      .offset = 0,
      .size = (VkDeviceSize)stride * (VkDeviceSize)h,
      .rowPitch = stride,
      .arrayPitch = 0,
      .depthPitch = 0,
  };
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod_explicit = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = modifier,
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &drm_layout,
  };

  int use_explicit = (modifier != 0 && modifier != IDK_DRM_FORMAT_MOD_INVALID);

  VkExternalMemoryImageCreateInfo ext_mem_ci = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = use_explicit ? &mod_explicit : NULL,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };

  VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &ext_mem_ci,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = vk_fmt,
      .extent = {w, h, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = use_explicit ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
  };

  r = vkCreateImage(dev, &ici, NULL, out_img);
  if (r != VK_SUCCESS && use_explicit) {
    IDK_LOG("comp-vk", "dmabuf: explicit modifier import failed (%d), trying LINEAR\n", r);
    ext_mem_ci.pNext = NULL;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    r = vkCreateImage(dev, &ici, NULL, out_img);
  }
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "dmabuf: CreateImage failed: %d (fmt=0x%x mod=0x%lx) - frame rejected\n", r, fourcc,
            (unsigned long)modifier);
    ctx->flags.vk_dmabuf_failed_this_frame = 1;
    return -1;
  }
  return 0;
}

/* Import a duplicate so every failure path keeps ownership of the caller's fd. */
static int dmabuf_import(int fd, VkImage img, uint32_t mem_type, VkDeviceSize size, VkDeviceMemory *out_mem) {
  vk_context_t *ctx = vk_ctx();
  VkDevice dev = ctx->device.vk_dev;
  VkResult r;
  VK_LOAD(vkAllocateMemory);
  VK_LOAD(vkBindImageMemory);
  VK_LOAD(vkDestroyImage);

  int import_fd = dup(fd);
  if (import_fd < 0) {
    ctx->flags.vk_dmabuf_failed_this_frame = 1;
    return -1;
  }

  VkImportMemoryFdInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = NULL,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = import_fd,
  };
  VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_info,
      .allocationSize = size,
      .memoryTypeIndex = mem_type,
  };
  r = vkAllocateMemory(dev, &mai, NULL, out_mem);
  if (r != VK_SUCCESS) {
    close(import_fd);
    IDK_ERR("comp-vk", "dmabuf: AllocateMemory(import) failed: %d - frame rejected\n", r);
    vkDestroyImage(dev, img, NULL);
    ctx->dmabuf.vk_dmabuf_img = VK_NULL_HANDLE;
    ctx->flags.vk_dmabuf_failed_this_frame = 1;
    return -1;
  }

  vkBindImageMemory(dev, img, *out_mem, 0);
  return 0;
}

/* Import a dmabuf fd as a VkImage and create a sampled view.
 * Returns 0 on success, -1 on failure. The caller retains ownership of fd;
 * the ICD imports a duplicate and the original is tracked for cache lifetime. */
int vk_upload_dmabuf(int fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t fourcc, uint64_t modifier,
                     uint16_t buf_id, VkCommandBuffer cmd) {
  vk_context_t *ctx = vk_ctx();
  VkDevice dev = ctx->device.vk_dev;
  VkResult r;
  VK_LOAD(vkCreateImageView);
  VK_LOAD(vkFreeMemory);
  VK_LOAD(vkDestroyImage);

  if (dmabuf_vendor_check(modifier) != 0)
    return -1;

  if (!dmabuf_check_cache(fd, w, h, stride, fourcc, modifier, buf_id)) {
    VkFormat vk_fmt = drm_fourcc_to_vk_format(fourcc);
    VkImage img = VK_NULL_HANDLE;
    if (dmabuf_create_image(w, h, stride, fourcc, modifier, vk_fmt, &img) != 0)
      return -1;
    ctx->dmabuf.vk_dmabuf_img = img;

    VkDeviceSize mem_size = 0;
    uint32_t mem_type = dmabuf_bind_memory(fd, img, &mem_size);
    if (mem_type == 0xFFFFFFFF) {
      vkDestroyImage(dev, img, NULL);
      ctx->dmabuf.vk_dmabuf_img = VK_NULL_HANDLE;
      ctx->flags.vk_dmabuf_failed_this_frame = 1;
      return -1;
    }

    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (dmabuf_import(fd, img, mem_type, mem_size, &mem) != 0)
      return -1;
    ctx->dmabuf.vk_dmabuf_img_mem = mem;

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = ctx->dmabuf.vk_dmabuf_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = vk_fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    r = vkCreateImageView(dev, &vci, NULL, &ctx->dmabuf.vk_dmabuf_view);
    if (r != VK_SUCCESS) {
      IDK_ERR("comp-vk", "dmabuf: CreateImageView failed: %d\n", r);
      vkFreeMemory(dev, ctx->dmabuf.vk_dmabuf_img_mem, NULL);
      ctx->dmabuf.vk_dmabuf_img_mem = VK_NULL_HANDLE;
      vkDestroyImage(dev, ctx->dmabuf.vk_dmabuf_img, NULL);
      ctx->dmabuf.vk_dmabuf_img = VK_NULL_HANDLE;
      ctx->dmabuf.vk_dmabuf_fd = -1;
      return -1;
    }

    ctx->dmabuf.vk_dmabuf_w = w;
    ctx->dmabuf.vk_dmabuf_h = h;
    ctx->dmabuf.vk_dmabuf_stride = stride;
    ctx->dmabuf.vk_dmabuf_fourcc = fourcc;
    ctx->dmabuf.vk_dmabuf_modifier = modifier;
    ctx->dmabuf.vk_dmabuf_fd = fd;
    ctx->dmabuf.vk_dmabuf_cache_id = buf_id;
    IDK_LOG("comp-vk", "dmabuf: imported %ux%u fourcc=0x%x mod=0x%lx stride=%u\n", w, h, fourcc,
            (unsigned long)modifier, stride);
  }

  dmabuf_barrier(cmd, ctx->dmabuf.vk_dmabuf_img);
  ctx->overlay.vk_overlay_img = ctx->dmabuf.vk_dmabuf_img;
  ctx->overlay.vk_overlay_view = ctx->dmabuf.vk_dmabuf_view;
  return 0;
}
