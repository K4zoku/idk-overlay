/* Instance-level hooks + GetInstanceProcAddr. */
#ifdef IDK_HAVE_VK_LAYER

#include <string.h>

#include "core/log.h"
#include "internal.h"

IDK_INTERNAL VKAPI_ATTR void VKAPI_CALL idk_DestroyInstance(VkInstance instance,
                                                            const VkAllocationCallbacks *pAllocator) {
  pthread_mutex_lock(&g_dispatch_lock);
  struct instance_dispatch *id = find_instance(instance);
  PFN_vkDestroyInstance fpDestroy = id ? id->DestroyInstance : NULL;
  remove_instance(instance);
  pthread_mutex_unlock(&g_dispatch_lock);

  if (fpDestroy)
    fpDestroy(instance, pAllocator);

  IDK_LOG("vk-layer", "DestroyInstance (instance=%p)\n", (void *)instance);
}

IDK_INTERNAL VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL idk_GetInstanceProcAddr(VkInstance instance, const char *pName) {
  if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
    return (PFN_vkVoidFunction)idk_GetInstanceProcAddr;
  if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)idk_GetDeviceProcAddr;

  static int s_gpa_log = 0;
  if (s_gpa_log < 20) {
    s_gpa_log++;
    IDK_LOG("vk-layer", "GetInstanceProcAddr(%s, inst=%p)\n", pName, (void *)instance);
  }

  for (size_t i = 0; g_instance_hooks[i].name; i++) {
    if (strcmp(pName, g_instance_hooks[i].name) == 0 && g_instance_hooks[i].ptr)
      return (PFN_vkVoidFunction)g_instance_hooks[i].ptr;
  }

  for (size_t i = 0; g_device_hooks[i].name; i++) {
    if (strcmp(pName, g_device_hooks[i].name) == 0 && g_device_hooks[i].ptr)
      return (PFN_vkVoidFunction)g_device_hooks[i].ptr;
  }

  if (instance) {
    pthread_mutex_lock(&g_dispatch_lock);
    struct instance_dispatch *id = find_instance(instance);
    PFN_vkGetInstanceProcAddr fp = id ? id->GetInstanceProcAddr : NULL;
    pthread_mutex_unlock(&g_dispatch_lock);
    if (fp)
      return fp(instance, pName);
  }

  return NULL;
}

#endif /* IDK_HAVE_VK_LAYER */
