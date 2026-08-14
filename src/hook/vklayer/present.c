/* Hook: vkQueuePresentKHR — render overlay on swapchain image before present. */
#ifdef IDK_HAVE_VK_LAYER

#include <stdlib.h>

#include "core/compositor_vk.h"
#include "core/log.h"
#include "internal.h"

IDK_INTERNAL VKAPI_ATTR VkResult VKAPI_CALL idk_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
  static int s_present_count = 0;
  s_present_count++;
  if (s_present_count % 300 == 1)
    IDK_LOG("vk-layer", "QueuePresentKHR (count=%d)\n", s_present_count);

  idk_vk_compositor_render();

  pthread_mutex_lock(&g_dispatch_lock);
  VkDevice device = find_device_for_queue(queue);
  struct device_dispatch *dd = device ? find_device(device) : NULL;
  PFN_vkQueuePresentKHR fp = dd ? dd->QueuePresentKHR : NULL;
  PFN_vkGetDeviceProcAddr gpa = dd ? dd->GetDeviceProcAddr : NULL;
  pthread_mutex_unlock(&g_dispatch_lock);

  if (!fp) {
    IDK_ERR("vk-layer", "QueuePresentKHR: no dispatch for queue=%p\n", (void *)queue);
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (g_overlay_visible && idk_vk_compositor_has_overlay() && pPresentInfo && pPresentInfo->swapchainCount > 0 && gpa) {
    VkSwapchainKHR sc = pPresentInfo->pSwapchains[0];
    uint32_t img_idx = pPresentInfo->pImageIndices[0];

    pthread_mutex_lock(&g_dispatch_lock);
    struct swapchain_data *sd = find_swapchain(sc);
    pthread_mutex_unlock(&g_dispatch_lock);

    if (sd && sd->width > 0 && sd->height > 0) {
      PFN_vkGetSwapchainImagesKHR fpGetImages = (PFN_vkGetSwapchainImagesKHR)gpa(device, "vkGetSwapchainImagesKHR");
      if (fpGetImages) {
        uint32_t img_count = 0;
        fpGetImages(device, sc, &img_count, NULL);
        if (img_count > 0 && img_idx < img_count) {
          VkImage *images = malloc(sizeof(VkImage) * img_count);
          if (images) {
            fpGetImages(device, sc, &img_count, images);
            VkImage swapchain_img = images[img_idx];
            free(images);

            PFN_vkAllocateCommandBuffers fpAllocCmd =
                (PFN_vkAllocateCommandBuffers)gpa(device, "vkAllocateCommandBuffers");
            PFN_vkBeginCommandBuffer fpBeginCmd = (PFN_vkBeginCommandBuffer)gpa(device, "vkBeginCommandBuffer");
            PFN_vkEndCommandBuffer fpEndCmd = (PFN_vkEndCommandBuffer)gpa(device, "vkEndCommandBuffer");
            PFN_vkQueueSubmit fpQueueSubmit = (PFN_vkQueueSubmit)gpa(device, "vkQueueSubmit");
            PFN_vkFreeCommandBuffers fpFreeCmd = (PFN_vkFreeCommandBuffers)gpa(device, "vkFreeCommandBuffers");

            if (fpAllocCmd && fpBeginCmd && fpEndCmd && fpQueueSubmit && fpFreeCmd) {
              idk_vk_compositor_render_overlay(VK_NULL_HANDLE, swapchain_img, sd->width, sd->height, sd->format);
            }
          }
        }
      }
    }
  }

  return fp(queue, pPresentInfo);
}

#endif /* IDK_HAVE_VK_LAYER */
