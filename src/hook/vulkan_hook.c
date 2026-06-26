#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>

#include "hook/syringe_hook.h"
#include "hook/hook_util.h"
#include "hook/hook_plugin.h"
#include "core/log.h"

/* Forward declaration — see compositor_vk.h for the real declaration.
 * We only need notify_resize here, so avoid pulling in the full Vulkan
 * header which may not be available in all build configs. */
void idk_vk_compositor_notify_resize(int w, int h);

typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef void* VkQueue;
typedef void* VkSwapchainKHR;
typedef void* VkImage;
typedef void* VkSemaphore;
typedef uint32_t VkResult;
typedef uint32_t VkStructureType;
typedef struct { uint32_t width, height; } VkExtent2D;

typedef struct VkSwapchainCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
    void *surface;
    uint32_t minImageCount;
    uint32_t imageFormat;
    uint32_t imageColorSpace;
    VkExtent2D imageExtent;
    uint32_t imageArrayLayers;
    uint32_t imageUsage;
    uint32_t imageSharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    uint32_t preTransform;
    uint32_t compositeAlpha;
    uint32_t presentMode;
    uint32_t clipped;
    void *oldSwapchain;
} VkSwapchainCreateInfoKHR;

typedef VkResult (*PFN_vkCreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR*,
                                              const void*, VkSwapchainKHR*);

#define VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT 10002286U

typedef struct VkPresentInfoKHR {
    VkStructureType sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore *pWaitSemaphores;
    uint32_t swapchainCount;
    const VkSwapchainKHR *pSwapchains;
    const uint32_t *pImageIndices;
    VkResult *pResults;
} VkPresentInfoKHR;

typedef VkResult (*PFN_vkQueuePresentKHR)(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
static PFN_vkQueuePresentKHR orig_QueuePresentKHR = NULL;
static int g_hook_installed = 0;
static int g_vk_swapchain_hook_installed = 0;

static PFN_vkCreateSwapchainKHR orig_CreateSwapchainKHR = NULL;

static int install_vk_swapchain_hook(void);

static VkResult vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (!orig_QueuePresentKHR)
        orig_QueuePresentKHR = (PFN_vkQueuePresentKHR)hook_orig("vkQueuePresentKHR");
    return orig_QueuePresentKHR(queue, pPresentInfo);
}

static VkResult vkCreateSwapchainKHR(VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const void *pAllocator, VkSwapchainKHR *pSwapchain)
{
    if (!orig_CreateSwapchainKHR)
        orig_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)hook_orig("vkCreateSwapchainKHR");

    VkResult ret = orig_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    if (ret == 0 && pCreateInfo)
        idk_vk_compositor_notify_resize((int)pCreateInfo->imageExtent.width,
                                        (int)pCreateInfo->imageExtent.height);
    return ret;
}

static int install_vk_hook(void) {
    if (g_hook_installed) return 0;

    int n = syringe_hook_install("vkQueuePresentKHR",
                                  (void *)vkQueuePresentKHR,
                                  (void **)&orig_QueuePresentKHR);
    if (n > 0) {
        g_hook_installed = 1;
        IDK_LOG("vk", "syringe hook installed for vkQueuePresentKHR\n");
    }
    return g_hook_installed ? 0 : -1;
}

static int install_vk_swapchain_hook(void) {
    if (g_vk_swapchain_hook_installed) return 0;

    int n = syringe_hook_install("vkCreateSwapchainKHR",
                                  (void *)vkCreateSwapchainKHR,
                                  (void **)&orig_CreateSwapchainKHR);
    if (n > 0) {
        g_vk_swapchain_hook_installed = 1;
        IDK_LOG("vk", "syringe hook installed for vkCreateSwapchainKHR\n");
    }
    return g_vk_swapchain_hook_installed ? 0 : -1;
}

int idk_vulkan_init(void) {
    if (g_hook_installed) return 0;

    if (install_vk_hook() != 0) return -1;
    install_vk_swapchain_hook();
    return 0;
}

void idk_vulkan_shutdown(void) {
    if (!g_hook_installed) return;
    syringe_hook_remove("vkQueuePresentKHR");
    syringe_hook_remove("vkCreateSwapchainKHR");
    g_hook_installed = 0;
    g_vk_swapchain_hook_installed = 0;
}

idk_hook_plugin_t idk_plugin_vk_syringe = {
    .name = "vk-syringe",
    .lib_patterns = {"libvulkan.so.1", "libvulkan.so", NULL},
    .init  = idk_vulkan_init,
    .shutdown = idk_vulkan_shutdown,
};
