/* Hook: vkCreateDevice — inject DMABUF import device extensions. */
#ifdef IDK_HAVE_VK_LAYER

#include <stdlib.h>
#include <string.h>

#include "core/compositor_vk.h"
#include "core/log.h"
#include "internal.h"

IDK_INTERNAL VKAPI_ATTR VkResult VKAPI_CALL idk_CreateDevice(VkPhysicalDevice physicalDevice,
                                                             const VkDeviceCreateInfo *pCreateInfo,
                                                             const VkAllocationCallbacks *pAllocator,
                                                             VkDevice *pDevice) {
  VkLayerDeviceCreateInfo *chain_info = idk_vk_layer_get_device_chain(pCreateInfo);
  if (!chain_info) {
    IDK_ERR("vk-layer", "CreateDevice: no layer link info in chain\n");
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;

  idk_vk_layer_chain_advance_device(chain_info);

  PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(NULL, "vkCreateDevice");
  if (!fpCreateDevice) {
    IDK_ERR("vk-layer", "CreateDevice: cannot find next vkCreateDevice\n");
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const char *dmabuf_exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
  const uint32_t dmabuf_ext_count = 2;

  uint32_t orig_ext_count = pCreateInfo ? pCreateInfo->enabledExtensionCount : 0;
  const char *const *orig_exts = pCreateInfo ? pCreateInfo->ppEnabledExtensionNames : NULL;

  const char **new_exts = (const char **)calloc(orig_ext_count + dmabuf_ext_count, sizeof(char *));
  uint32_t new_ext_count = 0;
  if (new_exts) {
    for (uint32_t i = 0; i < orig_ext_count; i++) {
      if (orig_exts[i])
        new_exts[new_ext_count++] = orig_exts[i];
    }
    for (uint32_t i = 0; i < dmabuf_ext_count; i++) {
      int dup = 0;
      for (uint32_t j = 0; j < new_ext_count; j++) {
        if (strcmp(new_exts[j], dmabuf_exts[i]) == 0) {
          dup = 1;
          break;
        }
      }
      if (!dup)
        new_exts[new_ext_count++] = dmabuf_exts[i];
    }
  }

  VkDeviceCreateInfo patched_ci;
  const VkDeviceCreateInfo *effective_ci = pCreateInfo;
  if (new_exts && new_ext_count > orig_ext_count) {
    patched_ci = *pCreateInfo;
    patched_ci.enabledExtensionCount = new_ext_count;
    patched_ci.ppEnabledExtensionNames = new_exts;
    effective_ci = &patched_ci;
    IDK_LOG("vk-layer", "CreateDevice: injected %u DMABUF ext(s) (total=%u)\n", new_ext_count - orig_ext_count,
            new_ext_count);
  }

  VkResult result = fpCreateDevice(physicalDevice, effective_ci, pAllocator, pDevice);
  free(new_exts);
  if (result != VK_SUCCESS)
    return result;

  pthread_mutex_lock(&g_dispatch_lock);
  struct device_dispatch *dd = new_device(*pDevice);
  pthread_mutex_unlock(&g_dispatch_lock);

  if (dd) {
    dd->GetDeviceProcAddr = fpGetDeviceProcAddr;
    dd->DestroyDevice = (PFN_vkDestroyDevice)fpGetDeviceProcAddr(*pDevice, "vkDestroyDevice");
    dd->CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)fpGetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR");
    dd->DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)fpGetDeviceProcAddr(*pDevice, "vkDestroySwapchainKHR");
    dd->QueuePresentKHR = (PFN_vkQueuePresentKHR)fpGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");
    dd->GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)fpGetDeviceProcAddr(*pDevice, "vkGetSwapchainImagesKHR");
    dd->GetDeviceQueue = (PFN_vkGetDeviceQueue)fpGetDeviceProcAddr(*pDevice, "vkGetDeviceQueue");
  }

  IDK_LOG("vk-layer", "CreateDevice OK (device=%p physDevice=%p)\n", (void *)*pDevice, (void *)physicalDevice);

  idk_vk_compositor_init(*pDevice, physicalDevice, 0, fpGetDeviceProcAddr, fpGetInstanceProcAddr);

  return result;
}

#endif /* IDK_HAVE_VK_LAYER */
