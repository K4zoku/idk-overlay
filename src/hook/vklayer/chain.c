/* Layer chain info helpers: walk pNext to find VK_LAYER_LINK_INFO and advance it. */
#ifdef IDK_HAVE_VK_LAYER

#include "internal.h"

static void *get_chain_info(const void *pNext, VkStructureType sType, VkLayerFunction func) {
  const VkBaseInStructure *item = (const VkBaseInStructure *)pNext;
  while (item) {
    if (item->sType == sType) {
      VkLayerInstanceCreateInfo *lic = (VkLayerInstanceCreateInfo *)item;
      if (lic->function == func)
        return (void *)item;
    }
    item = item->pNext;
  }
  return NULL;
}

VkLayerInstanceCreateInfo *idk_vk_layer_get_instance_chain(const VkInstanceCreateInfo *pCreateInfo) {
  return (VkLayerInstanceCreateInfo *)get_chain_info(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                                     VK_LAYER_LINK_INFO);
}

VkLayerDeviceCreateInfo *idk_vk_layer_get_device_chain(const VkDeviceCreateInfo *pCreateInfo) {
  return (VkLayerDeviceCreateInfo *)get_chain_info(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                                   VK_LAYER_LINK_INFO);
}

/* Advance the chain for the next layer */
void idk_vk_layer_chain_advance_instance(VkLayerInstanceCreateInfo *chain_info) {
  chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;
}

void idk_vk_layer_chain_advance_device(VkLayerDeviceCreateInfo *chain_info) {
  chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;
}

#endif /* IDK_HAVE_VK_LAYER */
