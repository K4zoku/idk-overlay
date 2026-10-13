/* render/vk upload.c - SHM frame upload: staging buffer + transfer image */

#include <string.h>

#include "public/idk_shm_delta.h"

#include "context.h"
#include "core/compositor.h"
#include "core/log.h"

/* (Re)create the host-visible staging buffer, growing it as needed. */
static int staging_ensure(VkDeviceSize buf_size) {
  vk_context_t *ctx = vk_ctx();
  VkDevice dev = ctx->device.vk_dev;
  vk_staging_t *st = &ctx->staging;
  VkResult r;
  VK_LOAD(vkCreateBuffer);
  VK_LOAD(vkAllocateMemory);
  VK_LOAD(vkBindBufferMemory);
  VK_LOAD(vkMapMemory);
  VK_LOAD(vkUnmapMemory);
  VK_LOAD(vkGetBufferMemoryRequirements);
  VK_LOAD(vkDestroyBuffer);
  VK_LOAD(vkFreeMemory);
  if (st->vk_staging_buf != VK_NULL_HANDLE && st->vk_staging_size >= buf_size)
    return 0;
  vkDestroyBuffer(dev, st->vk_staging_buf, NULL);
  st->vk_staging_buf = VK_NULL_HANDLE;
  if (st->vk_staging_mapped) {
    vkUnmapMemory(dev, st->vk_staging_mem);
    st->vk_staging_mapped = NULL;
  }
  vkFreeMemory(dev, st->vk_staging_mem, NULL);
  st->vk_staging_mem = VK_NULL_HANDLE;
  VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buf_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  r = vkCreateBuffer(dev, &bci, NULL, &st->vk_staging_buf);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "upload: CreateBuffer failed: %d\n", r);
    return -1;
  }
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev, st->vk_staging_buf, &mr);
  VkPhysicalDeviceMemoryProperties mp;
  ctx->gpa.vk_fn_GetPhysMemProps(ctx->device.vk_phys, &mp);
  uint32_t mem_type = find_mem_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  if (mem_type == 0xFFFFFFFF) {
    IDK_ERR("comp-vk", "upload: no HOST_VISIBLE mem type\n");
    return -1;
  }
  VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size,
      .memoryTypeIndex = mem_type,
  };
  r = vkAllocateMemory(dev, &mai, NULL, &st->vk_staging_mem);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "upload: AllocateMemory(staging) failed: %d\n", r);
    return -1;
  }
  vkBindBufferMemory(dev, st->vk_staging_buf, st->vk_staging_mem, 0);
  vkMapMemory(dev, st->vk_staging_mem, 0, buf_size, 0, &st->vk_staging_mapped);
  st->vk_staging_size = buf_size;
  return 0;
}

/* (Re)create the transfer-destination VkImage + view, sized to the frame.
 * Compare against vk_shm_img_w/h (the ACTUAL image dimensions), NOT
 * vk_overlay_w/h. */
static int image_ensure(uint32_t w, uint32_t h) {
  vk_context_t *ctx = vk_ctx();
  VkDevice dev = ctx->device.vk_dev;
  vk_staging_t *st = &ctx->staging;
  VkResult r;
  VK_LOAD(vkCreateImage);
  VK_LOAD(vkAllocateMemory);
  VK_LOAD(vkBindImageMemory);
  VK_LOAD(vkCreateImageView);
  VK_LOAD(vkGetImageMemoryRequirements);
  VK_LOAD(vkDestroyImage);
  VK_LOAD(vkDestroyImageView);
  VK_LOAD(vkFreeMemory);
  if (st->vk_shm_img != VK_NULL_HANDLE && ctx->overlay.vk_shm_img_w == w && ctx->overlay.vk_shm_img_h == h)
    return 0;
  st->vk_shm_baseline_valid = 0;
  vkDestroyImage(dev, st->vk_shm_img, NULL);
  st->vk_shm_img = VK_NULL_HANDLE;
  vkDestroyImageView(dev, st->vk_shm_view, NULL);
  st->vk_shm_view = VK_NULL_HANDLE;
  vkFreeMemory(dev, st->vk_shm_img_mem, NULL);
  st->vk_shm_img_mem = VK_NULL_HANDLE;
  VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {w, h, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  r = vkCreateImage(dev, &ici, NULL, &st->vk_shm_img);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "upload: CreateImage failed: %d\n", r);
    return -1;
  }
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, st->vk_shm_img, &mr);
  VkPhysicalDeviceMemoryProperties mp;
  ctx->gpa.vk_fn_GetPhysMemProps(ctx->device.vk_phys, &mp);
  uint32_t mem_type = find_mem_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mem_type == 0xFFFFFFFF) {
    IDK_ERR("comp-vk", "upload: no DEVICE_LOCAL mem type\n");
    return -1;
  }
  VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size,
      .memoryTypeIndex = mem_type,
  };
  r = vkAllocateMemory(dev, &mai, NULL, &st->vk_shm_img_mem);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "upload: AllocateMemory(image) failed: %d\n", r);
    return -1;
  }
  vkBindImageMemory(dev, st->vk_shm_img, st->vk_shm_img_mem, 0);
  VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = st->vk_shm_img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  r = vkCreateImageView(dev, &vci, NULL, &st->vk_shm_view);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "upload: CreateImageView failed: %d\n", r);
    return -1;
  }
  ctx->overlay.vk_shm_img_w = w;
  ctx->overlay.vk_shm_img_h = h;
  IDK_LOG("comp-vk", "upload: VkImage recreated (%ux%u)\n", w, h);
  return 0;
}

static int vk_upload_shm_data(const uint8_t *data, size_t bytes, uint32_t w, uint32_t h, VkCommandBuffer cmd) {
  vk_context_t *ctx = vk_ctx();
  VkDevice dev = ctx->device.vk_dev;
  VK_LOAD(vkFlushMappedMemoryRanges);
  if (!ctx->gpa.vk_fn_GetPhysMemProps || !data || bytes < (size_t)w * h * 4)
    return -1;
  if (staging_ensure(bytes) != 0)
    return -1;
  memcpy(ctx->staging.vk_staging_mapped, data, bytes);
  VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                               .memory = ctx->staging.vk_staging_mem, .size = VK_WHOLE_SIZE};
  if (vkFlushMappedMemoryRanges(dev, 1, &flush) != VK_SUCCESS || image_ensure(w, h) != 0)
    return -1;
  copy_buf_to_img(cmd, w, h);
  ctx->overlay.vk_overlay_img = ctx->staging.vk_shm_img;
  ctx->overlay.vk_overlay_view = ctx->staging.vk_shm_view;
  ctx->staging.vk_shm_baseline_valid = 1;
  return 0;
}

int vk_upload_shm(int fd, uint32_t w, uint32_t h, uint32_t pixel_size, uint32_t buf_idx, VkCommandBuffer cmd) {
  static idk_shm_cache_t cache;
  if (!idk_shm_cache_map(&cache, fd))
    return -1;
  return vk_upload_shm_data((const uint8_t *)cache.map + (buf_idx * pixel_size), pixel_size, w, h, cmd);
}

int vk_upload_shm_ring(const void *data, uint32_t w, uint32_t h, uint32_t pixel_size, VkCommandBuffer cmd) {
  return vk_upload_shm_data((const uint8_t *)data, pixel_size, w, h, cmd);
}

static int vk_upload_shm_rect_data(const uint8_t *pixels, size_t bytes, const idk_shm_delta_t *rect,
                                   VkCommandBuffer cmd) {
  vk_context_t *ctx = vk_ctx();
  if (!ctx->staging.vk_shm_baseline_valid || ctx->staging.vk_shm_img == VK_NULL_HANDLE ||
      staging_ensure(bytes) != 0)
    return -1;
  memcpy(ctx->staging.vk_staging_mapped, pixels, bytes);
  VK_LOAD(vkFlushMappedMemoryRanges);
  VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                               .memory = ctx->staging.vk_staging_mem, .size = VK_WHOLE_SIZE};
  if (vkFlushMappedMemoryRanges(ctx->device.vk_dev, 1, &flush) != VK_SUCCESS)
    return -1;
  VK_LOAD(vkCmdPipelineBarrier);
  VK_LOAD(vkCmdCopyBufferToImage);
  VkImageMemoryBarrier to_transfer = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = ctx->staging.vk_shm_img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, NULL, 0, NULL, 1, &to_transfer);
  VkBufferImageCopy copy = {
      .bufferOffset = 0,
      .bufferRowLength = 0, .bufferImageHeight = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageOffset = {(int32_t)rect->x, (int32_t)rect->y, 0},
      .imageExtent = {rect->width, rect->height, 1},
  };
  vkCmdCopyBufferToImage(cmd, ctx->staging.vk_staging_buf, ctx->staging.vk_shm_img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  VkImageMemoryBarrier to_sample = to_transfer;
  to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, NULL, 0, NULL, 1, &to_sample);
  ctx->overlay.vk_overlay_img = ctx->staging.vk_shm_img;
  ctx->overlay.vk_overlay_view = ctx->staging.vk_shm_view;
  return 0;
}

int vk_upload_shm_rect(int fd, uint32_t w, uint32_t h, VkCommandBuffer cmd) {
  vk_context_t *ctx = vk_ctx();
  static idk_shm_cache_t cache;
  if (!ctx->staging.vk_shm_baseline_valid || ctx->staging.vk_shm_img == VK_NULL_HANDLE ||
      ctx->overlay.vk_shm_img_w != w || ctx->overlay.vk_shm_img_h != h || !idk_shm_cache_map(&cache, fd) ||
      cache.size < sizeof(idk_shm_delta_t))
    return -1;
  const idk_shm_delta_t *rect = (const idk_shm_delta_t *)cache.map;
  if (!idk_shm_delta_validate(rect, w, h, cache.size))
    return -1;
  return vk_upload_shm_rect_data((const uint8_t *)cache.map + sizeof(*rect), cache.size - sizeof(*rect), rect, cmd);
}

int vk_upload_shm_ring_rect(const void *data, size_t bytes, uint32_t w, uint32_t h, VkCommandBuffer cmd) {
  vk_context_t *ctx = vk_ctx();
  if (!ctx->staging.vk_shm_baseline_valid || ctx->staging.vk_shm_img == VK_NULL_HANDLE ||
      ctx->overlay.vk_shm_img_w != w || ctx->overlay.vk_shm_img_h != h || !data ||
      bytes < sizeof(idk_shm_delta_t))
    return -1;
  const idk_shm_delta_t *rect = (const idk_shm_delta_t *)data;
  if (!idk_shm_delta_validate(rect, w, h, bytes))
    return -1;
  return vk_upload_shm_rect_data((const uint8_t *)data + sizeof(*rect), bytes - sizeof(*rect), rect, cmd);
}
