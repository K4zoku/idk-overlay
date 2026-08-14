#ifndef IDK_VKLAYER_INTERNAL_H
#define IDK_VKLAYER_INTERNAL_H

#ifdef IDK_HAVE_VK_LAYER

#include <pthread.h>
#include <stdatomic.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

/* Internal helpers: not part of the public symbol surface. */
#define IDK_INTERNAL __attribute__((visibility("hidden")))

#define MAX_INSTANCES 4
#define MAX_DEVICES 8
#define MAX_SWAPCHAINS 16
#define MAX_QUEUES 16

struct instance_dispatch {
  VkInstance instance;
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
  PFN_vkDestroyInstance DestroyInstance;
  int used;
};

struct device_dispatch {
  VkDevice device;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
  PFN_vkDestroyDevice DestroyDevice;
  PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
  PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
  PFN_vkQueuePresentKHR QueuePresentKHR;
  PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
  PFN_vkGetDeviceQueue GetDeviceQueue;
  int used;
};

struct swapchain_data {
  VkSwapchainKHR swapchain;
  VkDevice device;
  uint32_t width;
  uint32_t height;
  VkFormat format;
  int used;
};

struct queue_data {
  VkQueue queue;
  VkDevice device;
  int used;
};

/* Overlay visibility + webview liveness — defined in startup.c, exported. */
extern _Atomic int g_overlay_visible;
extern _Atomic int g_webview_dead;

/* Dispatch table storage (tables.c). */
IDK_INTERNAL extern pthread_mutex_t g_dispatch_lock;
IDK_INTERNAL struct instance_dispatch *find_instance(VkInstance inst);
IDK_INTERNAL struct instance_dispatch *new_instance(VkInstance inst);
IDK_INTERNAL void remove_instance(VkInstance inst);
IDK_INTERNAL struct device_dispatch *find_device(VkDevice dev);
IDK_INTERNAL struct device_dispatch *new_device(VkDevice dev);
IDK_INTERNAL void remove_device(VkDevice dev);
IDK_INTERNAL struct swapchain_data *find_swapchain(VkSwapchainKHR sc);
IDK_INTERNAL struct swapchain_data *new_swapchain(VkSwapchainKHR sc, VkDevice dev);
IDK_INTERNAL void remove_swapchain(VkSwapchainKHR sc);
IDK_INTERNAL struct queue_data *new_queue(VkQueue q, VkDevice dev);
IDK_INTERNAL VkDevice find_device_for_queue(VkQueue q);

/* Layer chain info helpers (chain.c). */
IDK_INTERNAL VkLayerInstanceCreateInfo *idk_vk_layer_get_instance_chain(const VkInstanceCreateInfo *pCreateInfo);
IDK_INTERNAL VkLayerDeviceCreateInfo *idk_vk_layer_get_device_chain(const VkDeviceCreateInfo *pCreateInfo);
IDK_INTERNAL void idk_vk_layer_chain_advance_instance(VkLayerInstanceCreateInfo *chain_info);
IDK_INTERNAL void idk_vk_layer_chain_advance_device(VkLayerDeviceCreateInfo *chain_info);

/* Hook entry points (referenced from the hook tables in tables.c). */
IDK_INTERNAL VKAPI_ATTR VkResult VKAPI_CALL idk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                               const VkAllocationCallbacks *pAllocator,
                                                               VkInstance *pInstance);
IDK_INTERNAL VKAPI_ATTR void VKAPI_CALL idk_DestroyInstance(VkInstance instance,
                                                            const VkAllocationCallbacks *pAllocator);
IDK_INTERNAL VKAPI_ATTR VkResult VKAPI_CALL idk_CreateDevice(VkPhysicalDevice physicalDevice,
                                                             const VkDeviceCreateInfo *pCreateInfo,
                                                             const VkAllocationCallbacks *pAllocator,
                                                             VkDevice *pDevice);
IDK_INTERNAL VKAPI_ATTR void VKAPI_CALL idk_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator);
IDK_INTERNAL VKAPI_ATTR VkResult VKAPI_CALL idk_CreateSwapchainKHR(VkDevice device,
                                                                   const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                                   const VkAllocationCallbacks *pAllocator,
                                                                   VkSwapchainKHR *pSwapchain);
IDK_INTERNAL VKAPI_ATTR void VKAPI_CALL idk_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                const VkAllocationCallbacks *pAllocator);
IDK_INTERNAL VKAPI_ATTR void VKAPI_CALL idk_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                                           uint32_t queueIndex, VkQueue *pQueue);
IDK_INTERNAL VKAPI_ATTR VkResult VKAPI_CALL idk_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);

/* GPA entry points (assigned by vkNegotiateLoaderLayerInterfaceVersion). */
IDK_INTERNAL VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL idk_GetInstanceProcAddr(VkInstance instance, const char *pName);
IDK_INTERNAL VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL idk_GetDeviceProcAddr(VkDevice device, const char *pName);

/* Hook tables (tables.c), NULL-terminated. */
struct hook_entry {
  const char *name;
  void *ptr;
};
IDK_INTERNAL extern struct hook_entry g_instance_hooks[];
IDK_INTERNAL extern struct hook_entry g_device_hooks[];

#endif /* IDK_HAVE_VK_LAYER */
#endif /* IDK_VKLAYER_INTERNAL_H */
