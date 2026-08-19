/* render/vk render.c - frame receive + overlay draw */

#include <stdatomic.h>
#include <unistd.h>

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
    ctx->dmabuf.vk_dmabuf_cache_id = 0;
    ctx->overlay.vk_overlay_w = hdr->width;
    ctx->overlay.vk_overlay_h = hdr->height;
    if (ctx->staging.vk_shm_fd >= 0)
      close(ctx->staging.vk_shm_fd);
    ctx->staging.vk_shm_fd = g_comp.dmabuf_fd[0];
    g_comp.dmabuf_fd[0] = -1;
    ctx->overlay.vk_has_frame = 1;
    if (ctx->dmabuf.vk_has_dmabuf_pending) {
      if (ctx->dmabuf.vk_dmabuf_pending_fd >= 0)
        close(ctx->dmabuf.vk_dmabuf_pending_fd);
      ctx->dmabuf.vk_has_dmabuf_pending = 0;
    }
    processed = 1;
  } else {
    if (ctx->dmabuf.vk_has_dmabuf_pending && ctx->dmabuf.vk_dmabuf_pending_fd >= 0) {
      close(ctx->dmabuf.vk_dmabuf_pending_fd);
    }
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
  if (processed)
    vk_send_ack(processed);
  return 0;
}

/* Create the graphics pipeline if needed - pass the actual swapchain format
 * so the render pass attachment format matches the swapchain image format. */
static int overlay_ensure_pipeline(VkFormat swapchainFormat) {
  vk_context_t *ctx = vk_ctx();
  if (swapchainFormat == VK_FORMAT_UNDEFINED) {
    swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
  }
  if (ctx->pipeline.vk_pipeline == VK_NULL_HANDLE || ctx->renderpass.vk_rp_format != swapchainFormat) {
    if (vk_create_pipeline(swapchainFormat) != 0) {
      IDK_ERR("comp-vk", "pipeline creation failed (fmt=%d)\n", (int)swapchainFormat);
      return -1;
    }
  }
  return 0;
}

/* Record the render pass + fullscreen-triangle draw for the current frame. */
static void overlay_draw(VkCommandBuffer cmd, VkFramebuffer fb, uint32_t width, uint32_t height) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkCmdBeginRenderPass);
  VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = ctx->renderpass.vk_render_pass,
      .framebuffer = fb,
      .renderArea = {{0, 0}, {width, height}},
  };
  vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

  VK_LOAD(vkCmdBindPipeline);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline.vk_pipeline);

  VkViewport vp = {.width = width, .height = height, .minDepth = 0, .maxDepth = 1};
  VK_LOAD(vkCmdSetViewport);
  vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D sc = {{0, 0}, {width, height}};
  VK_LOAD(vkCmdSetScissor);
  vkCmdSetScissor(cmd, 0, 1, &sc);

  VK_LOAD(vkCmdBindDescriptorSets);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline.vk_pll, 0, 1, &ctx->pipeline.vk_desc_set,
                          0, NULL);

  VK_LOAD(vkCmdDraw);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  VK_LOAD(vkCmdEndRenderPass);
  vkCmdEndRenderPass(cmd);
}

/* Mutex serializes render_overlay calls - the ring state (vk_async_ring_idx
 * / vk_async_ring[]) is not thread-safe. */
void idk_vk_compositor_render_overlay(VkCommandBuffer cmd, VkImage swapchainImage, uint32_t width, uint32_t height,
                                      VkFormat swapchainFormat) {
  (void)cmd;
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkEndCommandBuffer);
  VK_LOAD(vkFreeCommandBuffers);
  if (!ctx->overlay.vk_has_frame || swapchainImage == VK_NULL_HANDLE)
    return;

  pthread_mutex_lock(&ctx->flags.vk_render_lock);

  if (ring_cooldown_active())
    return;

  IDK_LOG("comp-vk", "render_overlay: has_frame=%d img=%p %ux%u fmt=%d\n", ctx->overlay.vk_has_frame,
          (void *)swapchainImage, width, height, (int)swapchainFormat);

  if (overlay_ensure_pipeline(swapchainFormat) != 0)
    return;

  VkCommandBuffer local_cmd = ring_begin_cmd();
  if (local_cmd == VK_NULL_HANDLE)
    return;
  VkCommandBuffer recording_cmd = local_cmd;

  if (overlay_upload(recording_cmd) != 0) {
    vkEndCommandBuffer(recording_cmd);
    vkFreeCommandBuffers(ctx->device.vk_dev, ctx->device.vk_cmd_pool, 1, &local_cmd);
    return;
  }

  if (ctx->overlay.vk_overlay_view == VK_NULL_HANDLE) {
    IDK_ERR("comp-vk", "overlay_view NULL - skipping render\n");
    vkEndCommandBuffer(recording_cmd);
    vkFreeCommandBuffers(ctx->device.vk_dev, ctx->device.vk_cmd_pool, 1, &local_cmd);
    return;
  }

  IDK_LOG("comp-vk", "render_overlay: proceeding with render pass\n");

  VkImageView swapchain_view;
  VkFramebuffer fb;
  if (renderpass_make_framebuffer(swapchainImage, width, height, &fb, &swapchain_view) != 0) {
    vkFreeCommandBuffers(ctx->device.vk_dev, ctx->device.vk_cmd_pool, 1, &local_cmd);
    return;
  }

  overlay_draw(recording_cmd, fb, width, height);
  vkEndCommandBuffer(recording_cmd);

  ring_submit(recording_cmd, fb, swapchain_view);

  pthread_mutex_unlock(&ctx->flags.vk_render_lock);
}

int idk_vk_compositor_has_overlay(void) { return vk_ctx()->overlay.vk_has_frame; }
