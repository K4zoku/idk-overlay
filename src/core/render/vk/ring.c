/* render/vk ring.c - 2-slot rolling-fence async submit ring */

#include "context.h"
#include "core/log.h"

/* Swapchain recreation storm cooldown: skip overlay rendering for
 * SWAPCHAIN_COOLDOWN_MS after each swapchain recreation (see
 * idk_vk_compositor_notify_swapchain_created). Returns 1 while cooling. */
int ring_cooldown_active(void) {
  vk_context_t *ctx = vk_ctx();
  if (ctx->flags.vk_last_swapchain_create_ts.tv_sec != 0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long delta_ms = (now.tv_sec - ctx->flags.vk_last_swapchain_create_ts.tv_sec) * 1000L +
                    (now.tv_nsec - ctx->flags.vk_last_swapchain_create_ts.tv_nsec) / 1000000L;
    if (delta_ms < VK_SWAPCHAIN_COOLDOWN_MS)
      return 1;
  }
  return 0;
}

/* Allocate + begin a primary command buffer for this frame.
 * Returns VK_NULL_HANDLE on failure. */
VkCommandBuffer ring_begin_cmd(void) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkAllocateCommandBuffers);
  VK_LOAD(vkFreeCommandBuffers);
  VK_LOAD(vkBeginCommandBuffer);
  VK_LOAD(vkEndCommandBuffer);

  VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx->device.vk_cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VkCommandBuffer local_cmd;
  if (vkAllocateCommandBuffers(ctx->device.vk_dev, &cbai, &local_cmd) != VK_SUCCESS) {
    IDK_ERR("comp-vk", "allocate cmd buffer failed\n");
    return VK_NULL_HANDLE;
  }

  VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (vkBeginCommandBuffer(local_cmd, &cbi) != VK_SUCCESS) {
    vkFreeCommandBuffers(ctx->device.vk_dev, ctx->device.vk_cmd_pool, 1, &local_cmd);
    return VK_NULL_HANDLE;
  }
  return local_cmd;
}

/* Async submit using a 2-slot rolling fence ring - avoids blocking the
 * game's QueuePresentKHR on GPU pipeline drain (vkWaitForFences).
 *
 * slot = &ring[ring_idx++ % 2]
 *   if slot->fence: wait(slot->fence)  # GPU finished N-2 frames ago
 *                   free(slot->cmd, slot->fb, slot->view)
 *                   reset(slot->fence)
 *   else: slot->fence = create  # first 2 frames
 *   submit(slot->fence)  # NO wait
 *   slot->cmd = cmd; slot->fb = fb; slot->view = view
 *
 * Frame N's GPU work runs in parallel with frame N+1's CPU work; we only
 * block when the GPU falls more than 1 frame behind (visible jank anyway).
 * Cleanup of cmd/fb/view is deferred to the next time we come back to this
 * slot - the GPU still references them until the fence signals.
 * On failure the resources are cleaned up immediately (sync fallback). */
int ring_submit(VkCommandBuffer cmd, VkFramebuffer fb, VkImageView view) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkQueueSubmit);
  VK_LOAD(vkWaitForFences);
  VK_LOAD(vkResetFences);
  VK_LOAD(vkCreateFence);
  VK_LOAD(vkDestroyFence);
  VK_LOAD(vkFreeCommandBuffers);
  VK_LOAD(vkDestroyFramebuffer);
  VK_LOAD(vkDestroyImageView);

  if (!ctx->ring.vk_async_ring_init) {
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < VK_ASYNC_RING_SIZE; i++) {
      if (vkCreateFence(ctx->device.vk_dev, &fci, NULL, &ctx->ring.vk_async_ring[i].fence) != VK_SUCCESS) {
        IDK_ERR("comp-vk", "async ring: vkCreateFence(%d) failed - falling back to sync\n", i);
        for (int j = 0; j < i; j++) {
          vkDestroyFence(ctx->device.vk_dev, ctx->ring.vk_async_ring[j].fence, NULL);
          ctx->ring.vk_async_ring[j].fence = VK_NULL_HANDLE;
        }
        VkFence sync_fence;
        if (vkCreateFence(ctx->device.vk_dev, &fci, NULL, &sync_fence) == VK_SUCCESS) {
          VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
          VkResult r = vkQueueSubmit(ctx->device.vk_queue, 1, &si, sync_fence);
          if (r == VK_SUCCESS)
            vkWaitForFences(ctx->device.vk_dev, 1, &sync_fence, VK_TRUE, 1000000000ULL);
          else
            IDK_ERR("comp-vk", "QueueSubmit failed: %d\n", r);
          vkDestroyFence(ctx->device.vk_dev, sync_fence, NULL);
        }
        vkDestroyFramebuffer(ctx->device.vk_dev, fb, NULL);
        vkDestroyImageView(ctx->device.vk_dev, view, NULL);
        vkFreeCommandBuffers(ctx->device.vk_dev, ctx->device.vk_cmd_pool, 1, &cmd);
        return -1;
      }
    }
    ctx->ring.vk_async_ring_init = 1;
    IDK_LOG("comp-vk", "async ring: %d fences created\n", VK_ASYNC_RING_SIZE);
  }

  int slot_idx = ctx->ring.vk_async_ring_idx;
  ctx->ring.vk_async_ring_idx = (ctx->ring.vk_async_ring_idx + 1) % VK_ASYNC_RING_SIZE;
  struct vk_async_slot *slot = &ctx->ring.vk_async_ring[slot_idx];

  if (slot->fence != VK_NULL_HANDLE && slot->cmd != VK_NULL_HANDLE) {
    VkResult wr = vkWaitForFences(ctx->device.vk_dev, 1, &slot->fence, VK_TRUE, 1000000000ULL);
    if (wr == VK_SUCCESS) {
      vkFreeCommandBuffers(ctx->device.vk_dev, ctx->device.vk_cmd_pool, 1, &slot->cmd);
      vkDestroyFramebuffer(ctx->device.vk_dev, slot->fb, NULL);
      vkDestroyImageView(ctx->device.vk_dev, slot->view, NULL);
      slot->cmd = VK_NULL_HANDLE;
      slot->fb = VK_NULL_HANDLE;
      slot->view = VK_NULL_HANDLE;
      if (vkResetFences(ctx->device.vk_dev, 1, &slot->fence) != VK_SUCCESS) {
        IDK_ERR("comp-vk", "async ring: vkResetFences failed - falling back to sync\n");
        vkDestroyFence(ctx->device.vk_dev, slot->fence, NULL);
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(ctx->device.vk_dev, &fci, NULL, &slot->fence);
      }
    } else {
      IDK_ERR("comp-vk", "async ring: wait returned %d - slot leak likely\n", (int)wr);
      slot->cmd = VK_NULL_HANDLE;
      slot->fb = VK_NULL_HANDLE;
      slot->view = VK_NULL_HANDLE;
      vkResetFences(ctx->device.vk_dev, 1, &slot->fence);
    }
  }

  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
  VkResult r = vkQueueSubmit(ctx->device.vk_queue, 1, &si, slot->fence);
  if (r == VK_SUCCESS) {
    slot->cmd = cmd;
    slot->fb = fb;
    slot->view = view;
    return 0;
  }
  IDK_ERR("comp-vk", "QueueSubmit failed: %d - falling back to immediate cleanup\n", (int)r);
  vkDestroyFramebuffer(ctx->device.vk_dev, fb, NULL);
  vkDestroyImageView(ctx->device.vk_dev, view, NULL);
  vkFreeCommandBuffers(ctx->device.vk_dev, ctx->device.vk_cmd_pool, 1, &cmd);
  return -1;
}

/* Shutdown: wait for any outstanding async-ring work and free deferred
 * per-frame resources (cmd buffer / framebuffer / image view) before the
 * device is destroyed. Also resets vk_async_ring_init. */
void ring_shutdown(void) {
  vk_context_t *ctx = vk_ctx();
  if (!ctx->ring.vk_async_ring_init || ctx->device.vk_dev == VK_NULL_HANDLE)
    return;
  VK_LOAD(vkWaitForFences);
  VK_LOAD(vkDestroyFence);
  VK_LOAD(vkFreeCommandBuffers);
  VK_LOAD(vkDestroyFramebuffer);
  VK_LOAD(vkDestroyImageView);
  for (int i = 0; i < VK_ASYNC_RING_SIZE; i++) {
    struct vk_async_slot *s = &ctx->ring.vk_async_ring[i];
    if (s->fence != VK_NULL_HANDLE) {
      if (s->cmd != VK_NULL_HANDLE) {
        vkWaitForFences(ctx->device.vk_dev, 1, &s->fence, VK_TRUE, 1000000000ULL);
        vkFreeCommandBuffers(ctx->device.vk_dev, ctx->device.vk_cmd_pool, 1, &s->cmd);
        vkDestroyFramebuffer(ctx->device.vk_dev, s->fb, NULL);
        vkDestroyImageView(ctx->device.vk_dev, s->view, NULL);
        s->cmd = VK_NULL_HANDLE;
        s->fb = VK_NULL_HANDLE;
        s->view = VK_NULL_HANDLE;
      }
      vkDestroyFence(ctx->device.vk_dev, s->fence, NULL);
      s->fence = VK_NULL_HANDLE;
    }
  }
  ctx->ring.vk_async_ring_init = 0;
  ctx->ring.vk_async_ring_idx = 0;
}
