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
#include "shim/elfhacks.h"

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
static int g_hook_installed = 0;
static int g_retry_done = 0;

static void *(*real_dlsym)(void *, const char *) = NULL;
static void *(*real_dlopen)(const char *, int) = NULL;
static int (*real_dlclose)(void *) = NULL;

static VkResult call_real_vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
static VkResult hook_QueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
static int install_vk_hook(void);

static void load_real_functions(void) {
    if (real_dlsym) return;

    eh_obj_t lib;
    const char *libs[] = {
#if defined(__GLIBC__)
        "*libdl.so*",
#endif
        "*libc.so*",
        "*libc.*.so*",
        "*ld-musl-*.so*",
    };

    for (size_t i = 0; i < sizeof(libs) / sizeof(*libs); i++) {
        if (eh_find_obj(&lib, libs[i]) != 0) continue;
        eh_find_sym(&lib, "dlsym", (void **)&real_dlsym);
        eh_find_sym(&lib, "dlopen", (void **)&real_dlopen);
        eh_find_sym(&lib, "dlclose", (void **)&real_dlclose);
        eh_destroy_obj(&lib);
        if (real_dlsym) break;
    }

    if (real_dlsym) {
        IDK_LOG("vk", "real functions loaded: dlsym=%p dlopen=%p\n",
                (void *)real_dlsym, (void *)real_dlopen);
    }
}

static VkResult call_real_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    load_real_functions();
    if (real_dlsym) {
        void *lib = dlopen("libvulkan.so.1", RTLD_LAZY);
        if (!lib) lib = dlopen("libvulkan.so", RTLD_LAZY);
        if (lib) {
            void *sym = real_dlsym(lib, "vkQueuePresentKHR");
            if (sym) {
                PFN_vkQueuePresentKHR fn = (PFN_vkQueuePresentKHR)sym;
                VkResult ret = fn(queue, pPresentInfo);
                if (real_dlclose) real_dlclose(lib);
                return ret;
            }
            if (real_dlclose) real_dlclose(lib);
        }
    }
    return 0;
}

static int install_vk_hook(void) {
    load_real_functions();
    if (g_hook_installed) return 0;
    g_hook_installed = 1;

    void *sym = NULL;
    static const char *vk_libs[] = {"libvulkan.so", "libvulkan.so.1", NULL};
    for (int i = 0; vk_libs[i]; i++) {
        void *h = real_dlopen ? real_dlopen(vk_libs[i], RTLD_NOW | RTLD_NOLOAD) : dlopen(vk_libs[i], RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = real_dlopen ? real_dlopen(vk_libs[i], RTLD_NOW | RTLD_GLOBAL) : dlopen(vk_libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            continue;
        sym = real_dlsym ? real_dlsym(h, "vkQueuePresentKHR") : dlsym(h, "vkQueuePresentKHR");
        if (sym) break;
    }
    if (!sym) {
        IDK_ERR("vk", "vkQueuePresentKHR not found");
        return -1;
    }

    int n = 0;
    n = syringe_hook_install_addr("vkQueuePresentKHR", sym,
                                   (void *)hook_QueuePresentKHR,
                                   (void **)&orig_QueuePresentKHR);
    if (n > 0) {
        IDK_LOG("vk", "installed via install_addr");
        return 0;
    }

    n = syringe_hook_install("vkQueuePresentKHR",
                             (void *)hook_QueuePresentKHR,
                             (void **)&orig_QueuePresentKHR);
    if (n > 0) {
        IDK_LOG("vk", "installed via GOT walk");
        return 0;
    }

    n = syringe_hook_install_addr("vkQueuePresentKHR", sym,
                                   (void *)hook_QueuePresentKHR,
                                   (void **)&orig_QueuePresentKHR);
    if (n > 0) {
        IDK_LOG("vk", "installed via install_addr (retry)");
        return 0;
    }

    IDK_ERR("vk", "all install methods failed, deferring to first call");
    return -1;
}

static VkResult hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (!orig_QueuePresentKHR && !g_retry_done) {
        g_retry_done = 1;
        install_vk_hook();
    }

    if (orig_QueuePresentKHR && pPresentInfo) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            (void)i;
        }
    }

    if (orig_QueuePresentKHR)
        return orig_QueuePresentKHR(queue, pPresentInfo);
    return call_real_vkQueuePresentKHR(queue, pPresentInfo);
}

int idk_vulkan_init(int ipc_fd, const char *socket_path) {
    g_ipc_fd = ipc_fd;
    (void)socket_path;
    if (g_hook_installed) return 0;
    return install_vk_hook();
}

void idk_vulkan_shutdown(void) {
    syringe_hook_remove("vkQueuePresentKHR");
    orig_QueuePresentKHR = NULL;
    g_hook_installed = 0;
    g_retry_done = 0;
}
