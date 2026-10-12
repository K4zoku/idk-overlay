/* render/vk render.c - frame receive + overlay draw */

#include <linux/dma-buf.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../linux/sync.h"
#include "public/idk_shm_delta.h"

#include "context.h"
#include "core/compositor.h"
#include "core/compositor_vk.h"
#include "core/log.h"

extern _Atomic int g_overlay_visible;
extern _Atomic int g_webview_dead;

static void vk_send_ack(int processed) {
  vk_context_t *ctx = vk_ctx();
  uint8_t code = (ctx->flags.vk_dmabuf_failed_this_frame || processed < 0) ? 1 : 0;
  ctx->flags.vk_dmabuf_failed_this_frame = 0;
  idk_compositor_send_ack(code);
}

static void vk_send_release_ack(int processed) {
  vk_context_t *ctx = vk_ctx();
  if (!ctx->flags.vk_pending_release_ack) {
    vk_send_ack(processed);
    return;
  }

  int sync_fd = idk_dmabuf_export_sync_file(ctx->dmabuf.vk_dmabuf_fd, DMA_BUF_SYNC_WRITE);
  if (sync_fd >= 0) {
    if (idk_sync_file_wait(sync_fd) != 0)
      IDK_ERR("comp-vk", "sync_file release wait failed\n");
    close(sync_fd);
  } else {
    VK_LOAD(vkQueueWaitIdle);
    if (vkQueueWaitIdle(ctx->device.vk_queue) != VK_SUCCESS)
      IDK_ERR("comp-vk", "queue idle fallback failed\n");
    IDK_LOG("comp-vk", "sync_file release export unavailable; used queue-idle fallback\n");
  }
  ctx->flags.vk_pending_release_ack = 0;
  vk_send_ack(processed);
}

int idk_vk_compositor_render(void) {
  vk_context_t *ctx = vk_ctx();
  if (g_webview_dead)
    return -1;

  int rc = idk_compositor_recv_frame(g_overlay_visible);
  if (rc <= 0)
    return -1;

  idk_frame_header_t *hdr = &g_comp.hdr;
  int processed = 0;

  if (!idk_frame_is_dmabuf(hdr)) {
    if (hdr->flags & IDK_FRAME_FLAG_SHM_RECT) {
      int fd = g_comp.dmabuf_fd[0];
      idk_shm_delta_t rect;
      struct stat st;
      if (!ctx->staging.vk_shm_baseline_valid ||
          ctx->overlay.vk_shm_img_w != hdr->width || ctx->overlay.vk_shm_img_h != hdr->height ||
          fd < 0 || fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(rect) ||
          pread(fd, &rect, sizeof(rect), 0) != (ssize_t)sizeof(rect) ||
          !idk_shm_delta_validate(&rect, hdr->width, hdr->height, (size_t)st.st_size)) {
        idk_compositor_close_frame_fds(g_comp.dmabuf_fd, &g_comp.nfd);
        idk_compositor_send_ack(1);
        return -1;
      }
    }
    ctx->dmabuf.vk_dmabuf_cache_id = 0;
    ctx->overlay.vk_overlay_w = hdr->width;
    ctx->overlay.vk_overlay_h = hdr->height;
    if (ctx->staging.vk_shm_fd >= 0)
      close(ctx->staging.vk_shm_fd);
    ctx->staging.vk_shm_fd = g_comp.dmabuf_fd[0];
    ctx->staging.vk_shm_is_delta = (hdr->flags & IDK_FRAME_FLAG_SHM_RECT) != 0;
    g_comp.dmabuf_fd[0] = -1;
    ctx->overlay.vk_has_frame = 1;
    if (ctx->dmabuf.vk_has_dmabuf_pending) {
      if (ctx->dmabuf.vk_dmabuf_pending_fd >= 0)
        close(ctx->dmabuf.vk_dmabuf_pending_fd);
      ctx->dmabuf.vk_has_dmabuf_pending = 0;
    }
    processed = 1;
  } else {
    ctx->staging.vk_shm_baseline_valid = 0;
    if (ctx->dmabuf.vk_has_dmabuf_pending && ctx->dmabuf.vk_dmabuf_pending_fd >= 0)
      close(ctx->dmabuf.vk_dmabuf_pending_fd);
    ctx->dmabuf.vk_dmabuf_pending_fd = g_comp.dmabuf_fd[0];
    g_comp.dmabuf_fd[0] = -1;
    ctx->dmabuf.vk_dmabuf_pending_w = hdr->width;
    ctx->dmabuf.vk_dmabuf_pending_h = hdr->height;
    ctx->dmabuf.vk_dmabuf_pending_stride = hdr->stride;
    ctx->dmabuf.vk_dmabuf_pending_fourcc = hdr->fourcc;
    ctx->dmabuf.vk_dmabuf_pending_modifier = hdr->modifier;
    ctx->dmabuf.vk_dmabuf_pending_buf_id = hdr->buf_id;
    ctx->dmabuf.vk_has_dmabuf_pending = 1;
    ctx->overlay.vk_has_frame = 1;
    if (ctx->staging.vk_shm_fd >= 0) {
      close(ctx->staging.vk_shm_fd);
      ctx->staging.vk_shm_fd = -1;
    }
    processed = 1;
    IDK_LOG("comp-vk", "DMABUF pending: %ux%u stride=%u mod=0x%lx\n", hdr->width, hdr->height, hdr->stride,
            (unsigned long)hdr->modifier);
  }

  idk_compositor_close_frame_fds(g_comp.dmabuf_fd, &g_comp.nfd);
  if (processed) {
    if (idk_frame_is_dmabuf(hdr) && idk_sync_file_enabled())
      ctx->flags.vk_pending_release_ack = 1;
    else
      vk_send_ack(processed);
  }
  return 0;
}

static int overlay_ensure_pipeline(VkFormat swapchainFormat) {
  vk_context_t *ctx = vk_ctx();
  if (swapchainFormat == VK_FORMAT_UNDEFINED)
    swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
  if (ctx->pipeline.vk_pipeline == VK_NULL_HANDLE || ctx->renderpass.vk_rp_format != swapchainFormat) {
    if (vk_create_pipeline(swapchainFormat) != 0) {
      IDK_ERR("comp-vk", "pipeline creation failed (fmt=%d)\n", (int)swapchainFormat);
      return -1;
    }
  }
  return 0;
}

static void overlay_draw(VkCommandBuffer cmd, VkFramebuffer fb, uint32_t width, uint32_t height) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkCmdBeginRenderPass);
  VK_LOAD(vkCmdBindPipeline);
  VK_LOAD(vkCmdBindDescriptorSets);
  VK_LOAD(vkCmdSetViewport);
  VK_LOAD(vkCmdSetScissor);
  VK_LOAD(vkCmdDraw);
  VK_LOAD(vkCmdEndRenderPass);
  VkClearValue clear = {.color = {{0, 0, 0, 0}}};
  VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                              .renderPass = ctx->renderpass.vk_render_pass,
                              .framebuffer = fb,
                              .renderArea = {{0, 0}, {width, height}},
                              .clearValueCount = 1,
                              .pClearValues = &clear};
  vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline.vk_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline.vk_pll, 0, 1,
                          &ctx->pipeline.vk_desc_set, 0, NULL);
  VkViewport viewport = {.x = 0, .y = 0, .width = (float)width, .height = (float)height, .minDepth = 0, .maxDepth = 1};
  VkRect2D scissor = {.offset = {0, 0}, .extent = {width, height}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);
}

void idk_vk_compositor_render_overlay(VkCommandBuffer cmd, VkImage swapchainImage, uint32_t width, uint32_t height,
                                      VkFormat swapchainFormat) {
  (void)cmd;
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkEndCommandBuffer);
  VK_LOAD(vkFreeCommandBuffers);
  if (!ctx->overlay.vk_has_frame || swapchainImage == VK_NULL_HANDLE)
    return;

  pthread_mutex_lock(&ctx->flags.vk_render_lock);
  int submit_rc = -1;
  VkCommandBuffer local_cmd = VK_NULL_HANDLE;
  VkImageView swapchain_view = VK_NULL_HANDLE;
  VkFramebuffer fb = VK_NULL_HANDLE;
  if (ring_cooldown_active() || overlay_ensure_pipeline(swapchainFormat) != 0)
    goto done;
  local_cmd = ring_begin_cmd();
  if (local_cmd == VK_NULL_HANDLE)
    goto done;
  if (overlay_upload(local_cmd) != 0)
    goto free_cmd;
  if (ctx->overlay.vk_overlay_view == VK_NULL_HANDLE) {
    IDK_ERR("comp-vk", "overlay_view NULL - skipping render\n");
    goto free_cmd;
  }
  if (renderpass_make_framebuffer(swapchainImage, width, height, &fb, &swapchain_view) != 0)
    goto free_cmd;
  overlay_draw(local_cmd, fb, width, height);
  vkEndCommandBuffer(local_cmd);
  submit_rc = ring_submit(local_cmd, fb, swapchain_view);
  local_cmd = VK_NULL_HANDLE;
  fb = VK_NULL_HANDLE;
  swapchain_view = VK_NULL_HANDLE;
  goto done;

free_cmd:
  vkFreeCommandBuffers(ctx->device.vk_dev, ctx->device.vk_cmd_pool, 1, &local_cmd);
done:
  if (ctx->flags.vk_pending_release_ack) {
    if (submit_rc == 0)
      vk_send_release_ack(0);
    else {
      ctx->flags.vk_pending_release_ack = 0;
      vk_send_ack(-1);
    }
  }
  pthread_mutex_unlock(&ctx->flags.vk_render_lock);
}

int idk_vk_compositor_has_overlay(void) { return vk_ctx()->overlay.vk_has_frame; }
