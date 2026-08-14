/* Device-level hooks (destroy/swapchain/queue tracking) + GetDeviceProcAddr. */
#ifdef IDK_HAVE_VK_LAYER

#include <string.h>

#include "core/compositor_vk.h"
#include "core/log.h"
#include "internal.h"

IDK_INTERNAL VKAPI_ATTR void VKAPI_CALL idk_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
  pthread_mutex_lock(&g_dispatch_lock);
  struct device_dispatch *dd = find_device(device);
  PFN_vkDestroyDevice fpDestroy = dd ? dd->DestroyDevice : NULL;
  remove_device(device);
  pthread_mutex_unlock(&g_dispatch_lock);

  if (fpDestroy)
    fpDestroy(device, pAllocator);

  IDK_LOG("vk-layer", "DestroyDevice (device=%p)\n", (void *)device);
}

IDK_INTERNAL VKAPI_ATTR VkResult VKAPI_CALL idk_CreateSwapchainKHR(VkDevice device,
                                                                   const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                                   const VkAllocationCallbacks *pAllocator,
                                                                   VkSwapchainKHR *pSwapchain) {
  pthread_mutex_lock(&g_dispatch_lock);
  struct device_dispatch *dd = find_device(device);
  PFN_vkCreateSwapchainKHR fp = dd ? dd->CreateSwapchainKHR : NULL;
  pthread_mutex_unlock(&g_dispatch_lock);

  if (!fp) {
    IDK_ERR("vk-layer", "CreateSwapchainKHR: no dispatch\n");
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkResult result = fp(device, pCreateInfo, pAllocator, pSwapchain);
  if (result != VK_SUCCESS)
    return result;

  if (pCreateInfo) {
    uint32_t w = pCreateInfo->imageExtent.width;
    uint32_t h = pCreateInfo->imageExtent.height;
    idk_vk_compositor_notify_resize((int)w, (int)h);
    idk_vk_compositor_notify_swapchain_created();

    pthread_mutex_lock(&g_dispatch_lock);
    struct swapchain_data *sd = new_swapchain(*pSwapchain, device);
    pthread_mutex_unlock(&g_dispatch_lock);
    if (sd) {
      sd->width = w;
      sd->height = h;
      sd->format = pCreateInfo->imageFormat;
    }

    IDK_LOG("vk-layer", "CreateSwapchainKHR OK (%ux%u format=%d swapchain=%p)\n", w, h, (int)pCreateInfo->imageFormat,
            (void *)*pSwapchain);
  }

  return result;
}

IDK_INTERNAL VKAPI_ATTR void VKAPI_CALL idk_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                const VkAllocationCallbacks *pAllocator) {
  pthread_mutex_lock(&g_dispatch_lock);
  struct device_dispatch *dd = find_device(device);
  PFN_vkDestroySwapchainKHR fp = dd ? dd->DestroySwapchainKHR : NULL;
  remove_swapchain(swapchain);
  pthread_mutex_unlock(&g_dispatch_lock);

  if (fp)
    fp(device, swapchain, pAllocator);

  IDK_LOG("vk-layer", "DestroySwapchainKHR (swapchain=%p)\n", (void *)swapchain);
}

/* Track device→queue mapping for dispatch */
IDK_INTERNAL VKAPI_ATTR void VKAPI_CALL idk_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                                           uint32_t queueIndex, VkQueue *pQueue) {
  pthread_mutex_lock(&g_dispatch_lock);
  struct device_dispatch *dd = find_device(device);
  PFN_vkGetDeviceQueue fp = dd ? dd->GetDeviceQueue : NULL;
  pthread_mutex_unlock(&g_dispatch_lock);

  if (fp)
    fp(device, queueFamilyIndex, queueIndex, pQueue);

  if (pQueue && *pQueue) {
    pthread_mutex_lock(&g_dispatch_lock);
    new_queue(*pQueue, device);
    pthread_mutex_unlock(&g_dispatch_lock);
  }
}

IDK_INTERNAL VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL idk_GetDeviceProcAddr(VkDevice device, const char *pName) {
  if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)idk_GetDeviceProcAddr;

  static int s_gpa_log = 0;
  if (s_gpa_log < 500) {
    s_gpa_log++;
    IDK_LOG("vk-layer", "GetDeviceProcAddr(%s)\n", pName);
  }

  for (size_t i = 0; g_device_hooks[i].name; i++) {
    if (strcmp(pName, g_device_hooks[i].name) == 0 && g_device_hooks[i].ptr)
      return (PFN_vkVoidFunction)g_device_hooks[i].ptr;
  }

  if (device) {
    pthread_mutex_lock(&g_dispatch_lock);
    struct device_dispatch *dd = find_device(device);
    PFN_vkGetDeviceProcAddr fp = dd ? dd->GetDeviceProcAddr : NULL;
    pthread_mutex_unlock(&g_dispatch_lock);
    if (fp)
      return fp(device, pName);
  }

  return NULL;
}

#endif /* IDK_HAVE_VK_LAYER */
