/* Layer negotiation: called by the Vulkan loader; provide our proc-addr functions. */
#ifdef IDK_HAVE_VK_LAYER

#include "core/log.h"
#include "internal.h"

/* Layer mode flag: set when vkNegotiateLoaderLayerInterfaceVersion is called */
static int g_vk_layer_active = 0;

int idk_vk_layer_is_active(void) { return g_vk_layer_active; }

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) {
  if (!pVersionStruct)
    return VK_ERROR_INITIALIZATION_FAILED;

  if (pVersionStruct->loaderLayerInterfaceVersion < 2)
    return VK_ERROR_INITIALIZATION_FAILED;

  pVersionStruct->loaderLayerInterfaceVersion = 2;
  pVersionStruct->pfnGetInstanceProcAddr = idk_GetInstanceProcAddr;
  pVersionStruct->pfnGetDeviceProcAddr = idk_GetDeviceProcAddr;
  pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;

  g_vk_layer_active = 1;

  IDK_LOG("vk-layer", "Layer negotiated (version 2, layer mode active)\n");
  return VK_SUCCESS;
}

#endif /* IDK_HAVE_VK_LAYER */
