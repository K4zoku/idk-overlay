/* render/vk cache.c - dmabuf import cache + per-frame upload + barriers */

#include <unistd.h>

#include "context.h"
#include "core/log.h"

/* Import cache: when the (w,h,stride,fourcc,modifier,buf_id) tuple matches
 * the currently-imported image, the producer wrote new content into the SAME
 * dmabuf - the incoming fd is redundant. Close it and reuse the cached
 * VkImage. The imported VkImage's own fd is tracked in vk_dmabuf_fd and MUST
 * stay open for the image's lifetime. Returns 1 on hit (overlay img/view
 * set), 0 on miss (old resources destroyed + cache fields reset). */
int dmabuf_check_cache(int fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t fourcc, uint64_t modifier,
                       uint16_t buf_id) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkDestroyImageView);
  VK_LOAD(vkDestroyImage);
  VK_LOAD(vkFreeMemory);

  if (ctx->dmabuf.vk_dmabuf_img != VK_NULL_HANDLE && ctx->dmabuf.vk_dmabuf_w == w && ctx->dmabuf.vk_dmabuf_h == h &&
      ctx->dmabuf.vk_dmabuf_stride == stride && ctx->dmabuf.vk_dmabuf_fourcc == fourcc &&
      ctx->dmabuf.vk_dmabuf_modifier == modifier && buf_id != 0 && ctx->dmabuf.vk_dmabuf_cache_id == buf_id) {
    close(fd);
    ctx->overlay.vk_overlay_img = ctx->dmabuf.vk_dmabuf_img;
    ctx->overlay.vk_overlay_view = ctx->dmabuf.vk_dmabuf_view;
    return 1;
  }

  IDK_LOG("comp-vk", "dmabuf cache miss: buf_id=%u cached=%u %ux%u fd=%d\n", buf_id, ctx->dmabuf.vk_dmabuf_cache_id, w,
          h, fd);
  if (ctx->dmabuf.vk_dmabuf_view) {
    vkDestroyImageView(ctx->device.vk_dev, ctx->dmabuf.vk_dmabuf_view, NULL);
    ctx->dmabuf.vk_dmabuf_view = VK_NULL_HANDLE;
  }
  if (ctx->dmabuf.vk_dmabuf_img) {
    vkDestroyImage(ctx->device.vk_dev, ctx->dmabuf.vk_dmabuf_img, NULL);
    ctx->dmabuf.vk_dmabuf_img = VK_NULL_HANDLE;
  }
  if (ctx->dmabuf.vk_dmabuf_img_mem) {
    vkFreeMemory(ctx->device.vk_dev, ctx->dmabuf.vk_dmabuf_img_mem, NULL);
    ctx->dmabuf.vk_dmabuf_img_mem = VK_NULL_HANDLE;
  }
  ctx->dmabuf.vk_dmabuf_fd = -1;
  ctx->dmabuf.vk_dmabuf_cache_id = 0;
  return 0;
}

/* Record the SHM copy: transition barriers + vkCmdCopyBufferToImage. */
void copy_buf_to_img(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkCmdCopyBufferToImage);
  VK_LOAD(vkCmdPipelineBarrier);

  VkImageMemoryBarrier bar = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = ctx->staging.vk_shm_img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &bar);

  VkBufferImageCopy copy = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {w, h, 1},
  };
  vkCmdCopyBufferToImage(cmd, ctx->staging.vk_staging_buf, ctx->staging.vk_shm_img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  VkImageMemoryBarrier bar2 = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = ctx->staging.vk_shm_img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL,
                       1, &bar2);
}

/* Transition the imported dmabuf image to SHADER_READ_ONLY for sampling.
 * Each frame needs the ownership transfer: release back to EXTERNAL, then
 * acquire from EXTERNAL. On cache hit only the acquire runs (the release
 * already happened in the previous frame's present); on first import the
 * image is in PREINITIALIZED layout. srcAccessMask=0, dstAccessMask=
 * SHADER_READ. Both EXTERNAL queue family indices must be set (not IGNORED)
 * for the ownership transfer to take effect. */
void dmabuf_barrier(VkCommandBuffer cmd, VkImage img) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkCmdPipelineBarrier);
  VkImageMemoryBarrier bar = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
      .dstQueueFamilyIndex = ctx->device.vk_queue_family,
      .image = img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                       NULL, 1, &bar);
}

/* Destroy the imported dmabuf VkImage + memory + view and close the tracked
 * + pending fds (shutdown). Caller waited for queue idle. */
void dmabuf_destroy(void) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkDestroyImageView);
  VK_LOAD(vkDestroyImage);
  VK_LOAD(vkFreeMemory);

  if (ctx->dmabuf.vk_dmabuf_view != VK_NULL_HANDLE)
    vkDestroyImageView(ctx->device.vk_dev, ctx->dmabuf.vk_dmabuf_view, NULL);
  ctx->dmabuf.vk_dmabuf_view = VK_NULL_HANDLE;
  if (ctx->dmabuf.vk_dmabuf_img != VK_NULL_HANDLE)
    vkDestroyImage(ctx->device.vk_dev, ctx->dmabuf.vk_dmabuf_img, NULL);
  ctx->dmabuf.vk_dmabuf_img = VK_NULL_HANDLE;
  if (ctx->dmabuf.vk_dmabuf_img_mem != VK_NULL_HANDLE)
    vkFreeMemory(ctx->device.vk_dev, ctx->dmabuf.vk_dmabuf_img_mem, NULL);
  ctx->dmabuf.vk_dmabuf_img_mem = VK_NULL_HANDLE;

  if (ctx->dmabuf.vk_dmabuf_pending_fd >= 0) {
    close(ctx->dmabuf.vk_dmabuf_pending_fd);
    ctx->dmabuf.vk_dmabuf_pending_fd = -1;
  }
  ctx->dmabuf.vk_has_dmabuf_pending = 0;

  if (ctx->dmabuf.vk_dmabuf_fd >= 0) {
    close(ctx->dmabuf.vk_dmabuf_fd);
    ctx->dmabuf.vk_dmabuf_fd = -1;
  }
}

/* Upload the current frame's texture (DMABUF zero-copy import or SHM staging
 * copy), recording layout barriers into cmd. Returns 0 on success. */
int overlay_upload(VkCommandBuffer cmd) {
  vk_context_t *ctx = vk_ctx();
  if (ctx->dmabuf.vk_has_dmabuf_pending && ctx->dmabuf.vk_dmabuf_pending_fd >= 0) {
    int pending_fd = ctx->dmabuf.vk_dmabuf_pending_fd;
    ctx->dmabuf.vk_dmabuf_pending_fd = -1;
    ctx->dmabuf.vk_has_dmabuf_pending = 0;

    if (vk_upload_dmabuf(pending_fd, ctx->dmabuf.vk_dmabuf_pending_w, ctx->dmabuf.vk_dmabuf_pending_h,
                         ctx->dmabuf.vk_dmabuf_pending_stride, ctx->dmabuf.vk_dmabuf_pending_fourcc,
                         ctx->dmabuf.vk_dmabuf_pending_modifier, ctx->dmabuf.vk_dmabuf_pending_buf_id, cmd) != 0) {
      IDK_ERR("comp-vk", "render_overlay: dmabuf import failed (fd=%d)\n", pending_fd);
      if (ctx->dmabuf.vk_dmabuf_fd == pending_fd) {
        close(pending_fd);
        ctx->dmabuf.vk_dmabuf_fd = -1;
      }
      return -1;
    }
  } else if (ctx->staging.vk_shm_fd >= 0) {
    if (vk_upload_shm(ctx->staging.vk_shm_fd, ctx->overlay.vk_overlay_w, ctx->overlay.vk_overlay_h,
                      ctx->overlay.vk_overlay_w * ctx->overlay.vk_overlay_h * 4, 0, cmd) != 0) {
      IDK_ERR("comp-vk", "render_overlay: SHM upload failed (vk_shm_fd=%d)\n", ctx->staging.vk_shm_fd);
      return -1;
    }
  } else {
    IDK_LOG("comp-vk", "render_overlay: no SHM or DMABUF data\n");
  }
  return 0;
}
