/* render/vk context.c - compositor context instance + lifecycle */

#include <time.h>
#include <unistd.h>

#include "context.h"
#include "core/compositor.h"
#include "core/compositor_vk.h"
#include "core/log.h"

/* Single instance of the Vulkan compositor state (context.h). */
static vk_context_t s_vk_ctx = {
    .staging = {.vk_shm_fd = -1},
    .dmabuf = {.vk_dmabuf_fd = -1, .vk_dmabuf_pending_fd = -1},
    .flags =
        {
            .vk_render_lock = PTHREAD_MUTEX_INITIALIZER,
        },
};

IDK_INTERNAL vk_context_t *vk_ctx(void) { return &s_vk_ctx; }

void idk_vk_compositor_notify_swapchain_created(void) {
  clock_gettime(CLOCK_MONOTONIC, &vk_ctx()->flags.vk_last_swapchain_create_ts);
}

void idk_vk_compositor_notify_resize(int w, int h) {
  idk_comp_notify_resize(&g_comp.game_w, &g_comp.game_h, &g_comp.size_pending, &g_comp.last_resize_ts, w, h, "comp-vk");
}

int idk_vk_compositor_init(VkDevice device, VkPhysicalDevice physDevice, uint32_t queueFamily,
                           PFN_vkGetDeviceProcAddr gpa, PFN_vkGetInstanceProcAddr instanceGpa) {
  vk_context_t *ctx = vk_ctx();
  ctx->device.vk_dev = device;
  ctx->device.vk_phys = physDevice;
  ctx->device.vk_queue_family = queueFamily;
  ctx->gpa.vk_gpa = gpa;

  extern VkInstance idk_vk_layer_get_instance(void);

  VkInstance inst = idk_vk_layer_get_instance();

  if (inst != VK_NULL_HANDLE && instanceGpa) {
    if (!ctx->gpa.vk_fn_GetPhysMemProps)
      ctx->gpa.vk_fn_GetPhysMemProps =
          (PFN_vkGetPhysicalDeviceMemoryProperties)instanceGpa(inst, "vkGetPhysicalDeviceMemoryProperties");
    IDK_LOG("comp-vk", "GetPhysMemProps loaded=%p (instance=%p gpa=%p)\n", (void *)ctx->gpa.vk_fn_GetPhysMemProps,
            (void *)inst, (void *)instanceGpa);
  } else {
    IDK_ERR("comp-vk", "No VkInstance or GPA available (inst=%p gpa=%p)\n", (void *)inst, (void *)instanceGpa);
  }

  VK_LOAD(vkGetDeviceQueue);
  vkGetDeviceQueue(ctx->device.vk_dev, ctx->device.vk_queue_family, 0, &ctx->device.vk_queue);

  if (inst != VK_NULL_HANDLE && instanceGpa) {
    PFN_vkGetPhysicalDeviceProperties fnGetPhysDevProps =
        (PFN_vkGetPhysicalDeviceProperties)instanceGpa(inst, "vkGetPhysicalDeviceProperties");
    if (fnGetPhysDevProps) {
      VkPhysicalDeviceProperties props;
      fnGetPhysDevProps(ctx->device.vk_phys, &props);
      ctx->device.vk_vk_vendor_id = props.vendorID;
      ctx->device.vk_drm_vendor_id = idk_vk_vendor_to_drm(props.vendorID);
      IDK_LOG("comp-vk", "GPU vendor: Vk=0x%x → DRM=0x%02x (%s)\n", props.vendorID, ctx->device.vk_drm_vendor_id,
              ctx->device.vk_drm_vendor_id == 0x03   ? "NVIDIA"
              : ctx->device.vk_drm_vendor_id == 0x01 ? "Intel"
              : ctx->device.vk_drm_vendor_id == 0x02 ? "AMD"
                                                     : "unknown");
    } else {
      IDK_ERR("comp-vk", "vkGetPhysicalDeviceProperties not loaded - cross-GPU detection disabled\n");
    }
  } else {
    IDK_ERR("comp-vk", "No instance/gpa for vendor detection (inst=%p gpa=%p)\n", (void *)inst, (void *)instanceGpa);
  }

  if (vk_create_pipeline_objects() != 0) {
    IDK_ERR("comp-vk", "Pipeline init failed\n");
    return -1;
  }

  if (idk_compositor_init() != 0) {
    IDK_ERR("comp-vk", "Compositor init failed\n");
    return -1;
  }

  IDK_LOG("comp-vk", "Initialized (device=%p phys=%p queue=%p)\n", (void *)ctx->device.vk_dev,
          (void *)ctx->device.vk_phys, (void *)ctx->device.vk_queue);
  return 0;
}

/* Destroy SHM upload resources: transfer image + view + memory, staging
 * buffer + memory, and the SHM mmap fd. Caller waited for queue idle. */
static void vk_upload_destroy(void) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkDestroyImageView);
  VK_LOAD(vkDestroyImage);
  VK_LOAD(vkFreeMemory);
  VK_LOAD(vkDestroyBuffer);

  if (ctx->staging.vk_shm_view != VK_NULL_HANDLE)
    vkDestroyImageView(ctx->device.vk_dev, ctx->staging.vk_shm_view, NULL);
  ctx->staging.vk_shm_view = VK_NULL_HANDLE;
  if (ctx->staging.vk_shm_img != VK_NULL_HANDLE)
    vkDestroyImage(ctx->device.vk_dev, ctx->staging.vk_shm_img, NULL);
  ctx->staging.vk_shm_img = VK_NULL_HANDLE;
  if (ctx->staging.vk_shm_img_mem != VK_NULL_HANDLE)
    vkFreeMemory(ctx->device.vk_dev, ctx->staging.vk_shm_img_mem, NULL);
  ctx->staging.vk_shm_img_mem = VK_NULL_HANDLE;

  if (ctx->staging.vk_staging_buf != VK_NULL_HANDLE)
    vkDestroyBuffer(ctx->device.vk_dev, ctx->staging.vk_staging_buf, NULL);
  ctx->staging.vk_staging_buf = VK_NULL_HANDLE;
  if (ctx->staging.vk_staging_mem != VK_NULL_HANDLE)
    vkFreeMemory(ctx->device.vk_dev, ctx->staging.vk_staging_mem, NULL);
  ctx->staging.vk_staging_mem = VK_NULL_HANDLE;
  ctx->staging.vk_staging_mapped = NULL;
  ctx->staging.vk_staging_size = 0;

  if (ctx->staging.vk_shm_fd >= 0) {
    close(ctx->staging.vk_shm_fd);
    ctx->staging.vk_shm_fd = -1;
  }
}

void idk_vk_compositor_shutdown(void) {
  vk_context_t *ctx = vk_ctx();

  ring_shutdown();

  if (ctx->device.vk_dev != VK_NULL_HANDLE) {
    VK_LOAD(vkQueueWaitIdle);
    if (ctx->device.vk_queue != VK_NULL_HANDLE)
      vkQueueWaitIdle(ctx->device.vk_queue);

    descset_destroy();
    renderpass_destroy();
    vk_upload_destroy();
    dmabuf_destroy();
  }

  ctx->overlay.vk_overlay_img = VK_NULL_HANDLE;
  ctx->overlay.vk_overlay_view = VK_NULL_HANDLE;
  ctx->overlay.vk_overlay_w = 0;
  ctx->overlay.vk_overlay_h = 0;
  ctx->overlay.vk_shm_img_w = 0;
  ctx->overlay.vk_shm_img_h = 0;
  ctx->dmabuf.vk_dmabuf_w = 0;
  ctx->dmabuf.vk_dmabuf_h = 0;
  ctx->dmabuf.vk_dmabuf_cache_id = 0;

  ctx->device.vk_dev = VK_NULL_HANDLE;
  ctx->device.vk_phys = VK_NULL_HANDLE;
  ctx->device.vk_queue = VK_NULL_HANDLE;
  ctx->device.vk_queue_family = 0;

  idk_compositor_shutdown();
  ctx->overlay.vk_has_frame = 0;
  IDK_LOG("comp-vk", "Shut down (Vulkan objects destroyed)\n");
}
