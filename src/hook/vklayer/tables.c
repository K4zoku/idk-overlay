/* Dispatch table storage: fixed-size map from VkInstance/VkDevice to function pointers. */
#ifdef IDK_HAVE_VK_LAYER

#include <string.h>

#include "internal.h"

static struct instance_dispatch g_instances[MAX_INSTANCES];
static struct device_dispatch g_devices[MAX_DEVICES];
static struct swapchain_data g_swapchains[MAX_SWAPCHAINS];
static struct queue_data g_queues[MAX_QUEUES];
pthread_mutex_t g_dispatch_lock = PTHREAD_MUTEX_INITIALIZER;

struct instance_dispatch *find_instance(VkInstance inst) {
  for (int i = 0; i < MAX_INSTANCES; i++)
    if (g_instances[i].used && g_instances[i].instance == inst)
      return &g_instances[i];
  return NULL;
}

struct instance_dispatch *new_instance(VkInstance inst) {
  for (int i = 0; i < MAX_INSTANCES; i++) {
    if (!g_instances[i].used) {
      g_instances[i].instance = inst;
      g_instances[i].used = 1;
      return &g_instances[i];
    }
  }
  return NULL;
}

void remove_instance(VkInstance inst) {
  struct instance_dispatch *id = find_instance(inst);
  if (id)
    memset(id, 0, sizeof(*id));
}

struct device_dispatch *find_device(VkDevice dev) {
  for (int i = 0; i < MAX_DEVICES; i++)
    if (g_devices[i].used && g_devices[i].device == dev)
      return &g_devices[i];
  return NULL;
}

struct device_dispatch *new_device(VkDevice dev) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!g_devices[i].used) {
      g_devices[i].device = dev;
      g_devices[i].used = 1;
      return &g_devices[i];
    }
  }
  return NULL;
}

void remove_device(VkDevice dev) {
  struct device_dispatch *dd = find_device(dev);
  if (dd)
    memset(dd, 0, sizeof(*dd));
  for (int i = 0; i < MAX_SWAPCHAINS; i++) {
    if (g_swapchains[i].used && g_swapchains[i].device == dev)
      memset(&g_swapchains[i], 0, sizeof(g_swapchains[i]));
  }
  for (int i = 0; i < MAX_QUEUES; i++) {
    if (g_queues[i].used && g_queues[i].device == dev)
      memset(&g_queues[i], 0, sizeof(g_queues[i]));
  }
}

struct swapchain_data *find_swapchain(VkSwapchainKHR sc) {
  for (int i = 0; i < MAX_SWAPCHAINS; i++)
    if (g_swapchains[i].used && g_swapchains[i].swapchain == sc)
      return &g_swapchains[i];
  return NULL;
}

struct swapchain_data *new_swapchain(VkSwapchainKHR sc, VkDevice dev) {
  for (int i = 0; i < MAX_SWAPCHAINS; i++) {
    if (!g_swapchains[i].used) {
      g_swapchains[i].swapchain = sc;
      g_swapchains[i].device = dev;
      g_swapchains[i].used = 1;
      return &g_swapchains[i];
    }
  }
  return NULL;
}

void remove_swapchain(VkSwapchainKHR sc) {
  struct swapchain_data *sd = find_swapchain(sc);
  if (sd)
    memset(sd, 0, sizeof(*sd));
}

static struct queue_data *find_queue(VkQueue q) {
  for (int i = 0; i < MAX_QUEUES; i++)
    if (g_queues[i].used && g_queues[i].queue == q)
      return &g_queues[i];
  return NULL;
}

struct queue_data *new_queue(VkQueue q, VkDevice dev) {
  for (int i = 0; i < MAX_QUEUES; i++) {
    if (!g_queues[i].used) {
      g_queues[i].queue = q;
      g_queues[i].device = dev;
      g_queues[i].used = 1;
      return &g_queues[i];
    }
  }
  return NULL;
}

VkDevice find_device_for_queue(VkQueue q) {
  struct queue_data *qd = find_queue(q);
  return qd ? qd->device : VK_NULL_HANDLE;
}

/* Hook tables consulted by GetInstanceProcAddr/GetDeviceProcAddr. */
struct hook_entry g_instance_hooks[] = {
    {"vkGetInstanceProcAddr", (void *)NULL},
    {"vkCreateInstance", (void *)idk_CreateInstance},
    {"vkDestroyInstance", (void *)idk_DestroyInstance},
    {"vkCreateDevice", (void *)idk_CreateDevice},
    {NULL, NULL},
};

struct hook_entry g_device_hooks[] = {
    {"vkGetDeviceProcAddr", (void *)NULL},
    {"vkDestroyDevice", (void *)idk_DestroyDevice},
    {"vkCreateSwapchainKHR", (void *)idk_CreateSwapchainKHR},
    {"vkDestroySwapchainKHR", (void *)idk_DestroySwapchainKHR},
    {"vkGetDeviceQueue", (void *)idk_GetDeviceQueue},
    {"vkQueuePresentKHR", (void *)idk_QueuePresentKHR},
    {NULL, NULL},
};

#endif /* IDK_HAVE_VK_LAYER */
