#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>

#include "hook/syringe_hook.h"
#include "hook/vulkan.h"
#include "public/idk_ipc.h"
#include "core/log.h"

typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef void* VkQueue;
typedef void* VkSwapchainKHR;
typedef void* VkImage;
typedef void* VkSemaphore;
typedef uint32_t VkResult;
typedef uint32_t VkStructureType;

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

static int g_ipc_fd = -1;

typedef VkResult (*PFN_vkQueuePresentKHR)(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
static PFN_vkQueuePresentKHR orig_QueuePresentKHR = NULL;

/* [TODO] export VkImage to dmabuf fd via VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
 * requires full Vulkan layer setup (instance/device dispatch tables).
 * For now, stub — expected to return -1. */
static int __attribute__((unused)) export_image_to_dmabuf(VkImage image, uint32_t width,
                                  uint32_t height, uint32_t *out_fd) {
    (void)image; (void)width; (void)height; (void)out_fd;
    return -1;
}

static VkResult hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (orig_QueuePresentKHR && pPresentInfo) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            /* [TODO] extract VkImage from swapchain[i] at pImageIndices[i],
             * lookup swapchain metadata (tracked via vkCreateSwapchainKHR hook),
             * then export_image_to_dmabuf and send via idk_ipc_send_frame */
            (void)i;
        }
    }

    if (orig_QueuePresentKHR)
        return orig_QueuePresentKHR(queue, pPresentInfo);
    return 0;
}

int idk_vulkan_init(int ipc_fd, const char *socket_path) {
    g_ipc_fd = ipc_fd;
    (void)socket_path;

    static const char *vk_libs[] = {"libvulkan.so", "libvulkan.so.1", NULL};
    void *sym = NULL;
    for (int i = 0; vk_libs[i]; i++) {
        void *h = dlopen(vk_libs[i], RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = dlopen(vk_libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            continue;
        sym = dlsym(h, "vkQueuePresentKHR");
        if (sym)
            break;
    }
    if (!sym) {
        IDK_ERR("vk", "vkQueuePresentKHR not found\n");
        return -1;
    }

    int n = syringe_hook_install_addr("vkQueuePresentKHR", sym,
                                       (void *)hook_QueuePresentKHR,
                                       (void **)&orig_QueuePresentKHR);
    if (n <= 0) {
        IDK_ERR("vk", "syringe_hook_install_addr failed\n");
        return -1;
    }

    return 0;
}

void idk_vulkan_shutdown(void) {
    syringe_hook_remove("vkQueuePresentKHR");
    orig_QueuePresentKHR = NULL;
}
