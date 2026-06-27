/* vulkan_layer.c — Vulkan Layer implementation for idk-overlay
 *
 * This file implements the official Vulkan Layer interface so that
 * libidk-overlay.so can be loaded as a Vulkan layer (via VK_LAYER_PATH +
 * manifest JSON).
 *
 * The layer hooks:
 * - vkCreateInstance: store dispatch table, init compositor + input hooks
 * - vkCreateSwapchainKHR: track swapchain extent (resize sync)
 * - vkQueuePresentKHR: render overlay before present
 */

/* Only compile when Vulkan layer headers are available */
#ifdef IDK_HAVE_VK_LAYER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "core/compositor_vk.h"
#include "core/log.h"
#include "hook/overlay.h"

/* Overlay visibility — defined in overlay.c. When 0, skip render_overlay
 * so the game's present goes through unmodified (matches the EGL/GLX
 * hook behavior in egl_hook.c / glx_hook.c). */
extern _Atomic int g_overlay_visible;

/* ── Layer mode flag ────────────────────────────────────────────────────
 * Set when vkNegotiateLoaderLayerInterfaceVersion is called. */
static int g_vk_layer_active = 0;

/* Stored during CreateInstance for compositor_vk to load instance-level functions */
static VkInstance g_vk_instance = VK_NULL_HANDLE;
static PFN_vkGetInstanceProcAddr g_vk_instance_gpa = NULL;

int idk_vk_layer_is_active(void) {
    return g_vk_layer_active;
}

VkInstance idk_vk_layer_get_instance(void) { return g_vk_instance; }
PFN_vkGetInstanceProcAddr idk_vk_layer_get_instance_gpa(void) { return g_vk_instance_gpa; }

/* ── Dispatch table storage ─────────────────────────────────────────────
 * Simple fixed-size map from VkInstance/VkDevice handle → dispatch table.
 * Each dispatch table stores the "next" layer's function pointers. */

#define MAX_INSTANCES  4
#define MAX_DEVICES    8
#define MAX_SWAPCHAINS 16

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

#define MAX_QUEUES 16

static struct instance_dispatch g_instances[MAX_INSTANCES];
static struct device_dispatch   g_devices[MAX_DEVICES];
static struct swapchain_data    g_swapchains[MAX_SWAPCHAINS];
static struct queue_data        g_queues[MAX_QUEUES];
static pthread_mutex_t g_dispatch_lock = PTHREAD_MUTEX_INITIALIZER;

static struct instance_dispatch *find_instance(VkInstance inst) {
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (g_instances[i].used && g_instances[i].instance == inst)
            return &g_instances[i];
    return NULL;
}

static struct device_dispatch *find_device(VkDevice dev) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i].used && g_devices[i].device == dev)
            return &g_devices[i];
    return NULL;
}

static struct swapchain_data *find_swapchain(VkSwapchainKHR sc) {
    for (int i = 0; i < MAX_SWAPCHAINS; i++)
        if (g_swapchains[i].used && g_swapchains[i].swapchain == sc)
            return &g_swapchains[i];
    return NULL;
}

static struct instance_dispatch *new_instance(VkInstance inst) {
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (!g_instances[i].used) {
            g_instances[i].instance = inst;
            g_instances[i].used = 1;
            return &g_instances[i];
        }
    }
    return NULL;
}

static struct device_dispatch *new_device(VkDevice dev) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            g_devices[i].device = dev;
            g_devices[i].used = 1;
            return &g_devices[i];
        }
    }
    return NULL;
}

static struct swapchain_data *new_swapchain(VkSwapchainKHR sc, VkDevice dev) {
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

static void remove_instance(VkInstance inst) {
    struct instance_dispatch *id = find_instance(inst);
    if (id) memset(id, 0, sizeof(*id));
}

static void remove_device(VkDevice dev) {
    struct device_dispatch *dd = find_device(dev);
    if (dd) memset(dd, 0, sizeof(*dd));
    /* Also remove swapchains and queues belonging to this device */
    for (int i = 0; i < MAX_SWAPCHAINS; i++) {
        if (g_swapchains[i].used && g_swapchains[i].device == dev)
            memset(&g_swapchains[i], 0, sizeof(g_swapchains[i]));
    }
    for (int i = 0; i < MAX_QUEUES; i++) {
        if (g_queues[i].used && g_queues[i].device == dev)
            memset(&g_queues[i], 0, sizeof(g_queues[i]));
    }
}

static void remove_swapchain(VkSwapchainKHR sc) {
    struct swapchain_data *sd = find_swapchain(sc);
    if (sd) memset(sd, 0, sizeof(*sd));
}

static struct queue_data *find_queue(VkQueue q) {
    for (int i = 0; i < MAX_QUEUES; i++)
        if (g_queues[i].used && g_queues[i].queue == q)
            return &g_queues[i];
    return NULL;
}

static struct queue_data *new_queue(VkQueue q, VkDevice dev) {
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

static VkDevice find_device_for_queue(VkQueue q) {
    struct queue_data *qd = find_queue(q);
    return qd ? qd->device : VK_NULL_HANDLE;
}

/* ── Layer chain info helpers ───────────────────────────────────────────
 * Walk the pNext chain to find VkLayerInstanceCreateInfo / VkLayerDeviceCreateInfo
 * with the specified function (e.g. VK_LAYER_LINK_INFO).
 *
 * There can be MULTIPLE LOADER_INSTANCE_CREATE_INFO entries in the chain
 * with different functions. We must find the one with function==VK_LAYER_LINK_INFO.
 * (MangoHud does the same — see get_instance_chain_info in vulkan.cpp) */

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

/* ── Hook: vkCreateInstance ───────────────────────────────────────────── */

static VKAPI_ATTR VkResult VKAPI_CALL idk_CreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkInstance *pInstance)
{
    VkLayerInstanceCreateInfo *chain_info = (VkLayerInstanceCreateInfo *)
        get_chain_info(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, VK_LAYER_LINK_INFO);

    if (!chain_info || chain_info->function != VK_LAYER_LINK_INFO) {
        IDK_ERR("vk-layer", "CreateInstance: no layer link info in chain\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
        chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance fpCreateInstance =
        (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!fpCreateInstance) {
        IDK_ERR("vk-layer", "CreateInstance: next layer has no vkCreateInstance\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    /* Advance the chain for the next layer */
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    /* Store dispatch table */
    pthread_mutex_lock(&g_dispatch_lock);
    struct instance_dispatch *id = new_instance(*pInstance);
    pthread_mutex_unlock(&g_dispatch_lock);

    if (id) {
        id->GetInstanceProcAddr = fpGetInstanceProcAddr;
        id->DestroyInstance = (PFN_vkDestroyInstance)
            fpGetInstanceProcAddr(*pInstance, "vkDestroyInstance");
    }

    IDK_LOG("vk-layer", "CreateInstance OK (instance=%p)\n", (void *)*pInstance);

    /* Store VkInstance + GetInstanceProcAddr for compositor_vk to use
     * for loading instance-level functions (vkGetPhysicalDeviceMemoryProperties). */
    g_vk_instance = *pInstance;
    g_vk_instance_gpa = fpGetInstanceProcAddr;

    /* Initialize compositor socket + install wayland input hooks.
     * Vulkan compositor pipeline init happens in CreateDevice (needs VkDevice). */
    idk_vk_compositor_notify_resize(0, 0); /* dummy to set path */
    idk_overlay_try_install_wayland_input();

    return result;
}

/* ── Hook: vkDestroyInstance ──────────────────────────────────────────── */

static VKAPI_ATTR void VKAPI_CALL idk_DestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks *pAllocator)
{
    pthread_mutex_lock(&g_dispatch_lock);
    struct instance_dispatch *id = find_instance(instance);
    PFN_vkDestroyInstance fpDestroy = id ? id->DestroyInstance : NULL;
    remove_instance(instance);
    pthread_mutex_unlock(&g_dispatch_lock);

    if (fpDestroy)
        fpDestroy(instance, pAllocator);

    IDK_LOG("vk-layer", "DestroyInstance (instance=%p)\n", (void *)instance);
}

/* ── Hook: vkCreateDevice ─────────────────────────────────────────────── */

static VKAPI_ATTR VkResult VKAPI_CALL idk_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice)
{
    VkLayerDeviceCreateInfo *chain_info = (VkLayerDeviceCreateInfo *)
        get_chain_info(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);

    if (!chain_info || chain_info->function != VK_LAYER_LINK_INFO) {
        IDK_ERR("vk-layer", "CreateDevice: no layer link info in chain\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr =
        chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    /* vkCreateDevice is an instance-level function, NOT device-level.
     * Get it via GetInstanceProcAddr (available in VkLayerDeviceLink_). */
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
        chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    /* Advance the chain for the next layer */
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    /* Get the next layer's vkCreateDevice via instance GPA (not device GPA) */
    PFN_vkCreateDevice fpCreateDevice =
        (PFN_vkCreateDevice)fpGetInstanceProcAddr(NULL, "vkCreateDevice");
    if (!fpCreateDevice) {
        IDK_ERR("vk-layer", "CreateDevice: cannot find next vkCreateDevice\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    /* Inject DMABUF import device extensions so the compositor can import
     * Qt's dmabuf-exported VkImage via VkImportMemoryFdInfoKHR.
     *
     * IMPORTANT: Only DEVICE extensions go here. VK_KHR_external_memory_capabilities
     * is an INSTANCE extension (type="instance" in vk.xml) and must NOT be
     * passed to vkCreateDevice — NVIDIA silently accepts but later pipeline
     * creation fails with VK_ERROR_UNKNOWN (-13).
     *
     * Device extensions needed:
     *   - VK_KHR_external_memory_fd: provides vkGetMemoryFdKHR + VkImportMemoryFdInfoKHR
     *   - VK_EXT_external_memory_dma_buf: provides VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
     *
     * The matching instance extension (VK_KHR_external_memory_capabilities)
     * is auto-enabled by the loader if any device ext depends on it, so we
     * don't need to explicitly add it to vkCreateInstance.
     *
     * We make a shallow copy of pCreateInfo with an enlarged extension
     * array — original pCreateInfo is const, callers don't expect mutation. */
    const char *dmabuf_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    const uint32_t dmabuf_ext_count = 2;

    /* Build new extension list: original + our 3 (deduped). */
    uint32_t orig_ext_count = pCreateInfo ? pCreateInfo->enabledExtensionCount : 0;
    const char *const *orig_exts = pCreateInfo ? pCreateInfo->ppEnabledExtensionNames : NULL;

    /* Worst-case size: orig + 3. We'll dedupe in-place. */
    const char **new_exts = (const char **)calloc(orig_ext_count + dmabuf_ext_count, sizeof(char *));
    uint32_t new_ext_count = 0;
    if (new_exts) {
        for (uint32_t i = 0; i < orig_ext_count; i++) {
            if (orig_exts[i]) new_exts[new_ext_count++] = orig_exts[i];
        }
        for (uint32_t i = 0; i < dmabuf_ext_count; i++) {
            int dup = 0;
            for (uint32_t j = 0; j < new_ext_count; j++) {
                if (strcmp(new_exts[j], dmabuf_exts[i]) == 0) { dup = 1; break; }
            }
            if (!dup) new_exts[new_ext_count++] = dmabuf_exts[i];
        }
    }

    VkDeviceCreateInfo patched_ci;
    const VkDeviceCreateInfo *effective_ci = pCreateInfo;
    if (new_exts && new_ext_count > orig_ext_count) {
        patched_ci = *pCreateInfo;
        patched_ci.enabledExtensionCount = new_ext_count;
        patched_ci.ppEnabledExtensionNames = new_exts;
        effective_ci = &patched_ci;
        IDK_LOG("vk-layer", "CreateDevice: injected %u DMABUF ext(s) (total=%u)\n",
                new_ext_count - orig_ext_count, new_ext_count);
    }

    VkResult result = fpCreateDevice(physicalDevice, effective_ci, pAllocator, pDevice);
    free(new_exts);
    if (result != VK_SUCCESS) return result;

    /* Store dispatch table */
    pthread_mutex_lock(&g_dispatch_lock);
    struct device_dispatch *dd = new_device(*pDevice);
    pthread_mutex_unlock(&g_dispatch_lock);

    if (dd) {
        dd->GetDeviceProcAddr = fpGetDeviceProcAddr;
        dd->DestroyDevice = (PFN_vkDestroyDevice)
            fpGetDeviceProcAddr(*pDevice, "vkDestroyDevice");
        dd->CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)
            fpGetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR");
        dd->DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)
            fpGetDeviceProcAddr(*pDevice, "vkDestroySwapchainKHR");
        dd->QueuePresentKHR = (PFN_vkQueuePresentKHR)
            fpGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");
        dd->GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)
            fpGetDeviceProcAddr(*pDevice, "vkGetSwapchainImagesKHR");
        dd->GetDeviceQueue = (PFN_vkGetDeviceQueue)
            fpGetDeviceProcAddr(*pDevice, "vkGetDeviceQueue");
    }

    IDK_LOG("vk-layer", "CreateDevice OK (device=%p physDevice=%p)\n",
            (void *)*pDevice, (void *)physicalDevice);

    /* Initialize Vulkan compositor (pipeline, shaders, socket).
     * Pass the physical device + both device and instance GPAs
     * so compositor can load both device and instance-level functions. */
    idk_vk_compositor_init(*pDevice, physicalDevice, 0,
                           fpGetDeviceProcAddr, fpGetInstanceProcAddr);

    return result;
}

/* ── Hook: vkDestroyDevice ────────────────────────────────────────────── */

static VKAPI_ATTR void VKAPI_CALL idk_DestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks *pAllocator)
{
    pthread_mutex_lock(&g_dispatch_lock);
    struct device_dispatch *dd = find_device(device);
    PFN_vkDestroyDevice fpDestroy = dd ? dd->DestroyDevice : NULL;
    remove_device(device);
    pthread_mutex_unlock(&g_dispatch_lock);

    if (fpDestroy)
        fpDestroy(device, pAllocator);

    IDK_LOG("vk-layer", "DestroyDevice (device=%p)\n", (void *)device);
}

/* ── Hook: vkCreateSwapchainKHR ───────────────────────────────────────── */

static VKAPI_ATTR VkResult VKAPI_CALL idk_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain)
{
    pthread_mutex_lock(&g_dispatch_lock);
    struct device_dispatch *dd = find_device(device);
    PFN_vkCreateSwapchainKHR fp = dd ? dd->CreateSwapchainKHR : NULL;
    pthread_mutex_unlock(&g_dispatch_lock);

    if (!fp) {
        IDK_ERR("vk-layer", "CreateSwapchainKHR: no dispatch\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = fp(device, pCreateInfo, pAllocator, pSwapchain);
    if (result != VK_SUCCESS) return result;

    /* Track swapchain extent for resize sync.
     * CRITICAL: must call idk_vk_compositor_notify_resize (the VK path),
     * NOT only idk_compositor_notify_resize (the GL/EGL path). The VK
     * compositor embeds the swapchain extent in its ACK to the webview;
     * if it never receives a resize notification, the webview stays at
     * its default 1920x1080 and the overlay gets stretched onto the
     * (smaller) swapchain. */
    if (pCreateInfo) {
        uint32_t w = pCreateInfo->imageExtent.width;
        uint32_t h = pCreateInfo->imageExtent.height;
        idk_vk_compositor_notify_resize((int)w, (int)h);
        idk_vk_compositor_notify_swapchain_created();  /* skip overlay during storm */

        pthread_mutex_lock(&g_dispatch_lock);
        struct swapchain_data *sd = new_swapchain(*pSwapchain, device);
        pthread_mutex_unlock(&g_dispatch_lock);
        if (sd) {
            sd->width = w;
            sd->height = h;
            sd->format = pCreateInfo->imageFormat;
        }

        IDK_LOG("vk-layer", "CreateSwapchainKHR OK (%ux%u format=%d swapchain=%p)\n",
                w, h, (int)pCreateInfo->imageFormat, (void *)*pSwapchain);
    }

    return result;
}

/* ── Hook: vkDestroySwapchainKHR ──────────────────────────────────────── */

static VKAPI_ATTR void VKAPI_CALL idk_DestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator)
{
    pthread_mutex_lock(&g_dispatch_lock);
    struct device_dispatch *dd = find_device(device);
    PFN_vkDestroySwapchainKHR fp = dd ? dd->DestroySwapchainKHR : NULL;
    remove_swapchain(swapchain);
    pthread_mutex_unlock(&g_dispatch_lock);

    if (fp)
        fp(device, swapchain, pAllocator);

    IDK_LOG("vk-layer", "DestroySwapchainKHR (swapchain=%p)\n", (void *)swapchain);
}

/* ── Hook: vkGetDeviceQueue ─────────────────────────────────────────────
 * We need to track which device owns each queue so QueuePresentKHR
 * can find the correct dispatch table. */

static VKAPI_ATTR void VKAPI_CALL idk_GetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue *pQueue)
{
    pthread_mutex_lock(&g_dispatch_lock);
    struct device_dispatch *dd = find_device(device);
    PFN_vkGetDeviceQueue fp = dd ? dd->GetDeviceQueue : NULL;
    pthread_mutex_unlock(&g_dispatch_lock);

    if (fp)
        fp(device, queueFamilyIndex, queueIndex, pQueue);

    /* Map queue → device */
    if (pQueue && *pQueue) {
        pthread_mutex_lock(&g_dispatch_lock);
        new_queue(*pQueue, device);
        pthread_mutex_unlock(&g_dispatch_lock);
    }
}

/* ── Hook: vkQueuePresentKHR ────────────────────────────────────────────
 * Main hook point — render overlay on swapchain image before present.
 *
 * Flow:
 * 1. Receive webview frame (idk_vk_compositor_render)
 * 2. Get swapchain image index
 * 3. Allocate + begin command buffer
 * 4. idk_vk_compositor_render_overlay(cmd, swapchainImage, w, h)
 * 5. End + submit command buffer (with wait semaphores from present info)
 * 6. Forward to real QueuePresentKHR */

static VKAPI_ATTR VkResult VKAPI_CALL idk_QueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR *pPresentInfo)
{
    /* Receive webview frame (non-blocking) */
    idk_vk_compositor_render();

    static int s_present_count = 0;
    s_present_count++;
    if (s_present_count % 300 == 1) {
        IDK_LOG("vk-layer", "QueuePresentKHR (count=%d)\n", s_present_count);
    }

    /* Find device + dispatch table for this queue */
    pthread_mutex_lock(&g_dispatch_lock);
    VkDevice device = find_device_for_queue(queue);
    struct device_dispatch *dd = device ? find_device(device) : NULL;
    PFN_vkQueuePresentKHR fp = dd ? dd->QueuePresentKHR : NULL;
    PFN_vkGetDeviceProcAddr gpa = dd ? dd->GetDeviceProcAddr : NULL;
    pthread_mutex_unlock(&g_dispatch_lock);

    if (!fp) {
        IDK_ERR("vk-layer", "QueuePresentKHR: no dispatch for queue=%p\n", (void *)queue);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    /* Render overlay if we have a frame + swapchain info + overlay visible.
     * g_overlay_visible is the same flag EGL/GLX paths check in their
     * swap hooks — when 0, the game's present goes through unmodified. */
    if (g_overlay_visible &&
        idk_vk_compositor_has_overlay() &&
        pPresentInfo && pPresentInfo->swapchainCount > 0 && gpa) {
        /* Get swapchain + image index */
        VkSwapchainKHR sc = pPresentInfo->pSwapchains[0];
        uint32_t img_idx = pPresentInfo->pImageIndices[0];

        pthread_mutex_lock(&g_dispatch_lock);
        struct swapchain_data *sd = find_swapchain(sc);
        pthread_mutex_unlock(&g_dispatch_lock);

        if (sd && sd->width > 0 && sd->height > 0) {
            /* Get swapchain images */
            PFN_vkGetSwapchainImagesKHR fpGetImages =
                (PFN_vkGetSwapchainImagesKHR)gpa(device, "vkGetSwapchainImagesKHR");
            if (fpGetImages) {
                uint32_t img_count = 0;
                fpGetImages(device, sc, &img_count, NULL);
                if (img_count > 0 && img_idx < img_count) {
                    VkImage *images = malloc(sizeof(VkImage) * img_count);
                    if (images) {
                        fpGetImages(device, sc, &img_count, images);
                        VkImage swapchain_img = images[img_idx];
                        free(images);

                        /* Allocate command buffer */
                        PFN_vkAllocateCommandBuffers fpAllocCmd =
                            (PFN_vkAllocateCommandBuffers)gpa(device, "vkAllocateCommandBuffers");
                        PFN_vkBeginCommandBuffer fpBeginCmd =
                            (PFN_vkBeginCommandBuffer)gpa(device, "vkBeginCommandBuffer");
                        PFN_vkEndCommandBuffer fpEndCmd =
                            (PFN_vkEndCommandBuffer)gpa(device, "vkEndCommandBuffer");
                        PFN_vkQueueSubmit fpQueueSubmit =
                            (PFN_vkQueueSubmit)gpa(device, "vkQueueSubmit");
                        PFN_vkFreeCommandBuffers fpFreeCmd =
                            (PFN_vkFreeCommandBuffers)gpa(device, "vkFreeCommandBuffers");

                        if (fpAllocCmd && fpBeginCmd && fpEndCmd && fpQueueSubmit && fpFreeCmd) {
                            /* For Phase 2 initial test: just call render_overlay
                             * which will handle cmd buffer internally.
                             * TODO: proper command buffer + semaphore synchronization */
                            idk_vk_compositor_render_overlay(VK_NULL_HANDLE, swapchain_img,
                                                              sd->width, sd->height,
                                                              sd->format);
                        }
                    }
                }
            }
        }
    }

    return fp(queue, pPresentInfo);
}

/* ── GetInstanceProcAddr ──────────────────────────────────────────────── */

static const struct {
    const char *name;
    void *ptr;
} g_instance_hooks[] = {
    { "vkGetInstanceProcAddr",     (void *)NULL }, /* filled at runtime */
    { "vkCreateInstance",          (void *)idk_CreateInstance },
    { "vkDestroyInstance",         (void *)idk_DestroyInstance },
    { "vkCreateDevice",            (void *)idk_CreateDevice },
};

/* ── GetDeviceProcAddr ────────────────────────────────────────────────── */

static const struct {
    const char *name;
    void *ptr;
} g_device_hooks[] = {
    { "vkGetDeviceProcAddr",       (void *)NULL }, /* filled at runtime */
    { "vkDestroyDevice",           (void *)idk_DestroyDevice },
    { "vkCreateSwapchainKHR",      (void *)idk_CreateSwapchainKHR },
    { "vkDestroySwapchainKHR",     (void *)idk_DestroySwapchainKHR },
    { "vkGetDeviceQueue",          (void *)idk_GetDeviceQueue },
    { "vkQueuePresentKHR",         (void *)idk_QueuePresentKHR },
};

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL idk_GetInstanceProcAddr(
    VkInstance instance,
    const char *pName)
{
    /* Check our instance hooks */
    for (size_t i = 0; i < sizeof(g_instance_hooks) / sizeof(g_instance_hooks[0]); i++) {
        if (strcmp(pName, g_instance_hooks[i].name) == 0 && g_instance_hooks[i].ptr)
            return (PFN_vkVoidFunction)g_instance_hooks[i].ptr;
    }

    /* Check device hooks (some are accessible via instance GPA) */
    for (size_t i = 0; i < sizeof(g_device_hooks) / sizeof(g_device_hooks[0]); i++) {
        if (strcmp(pName, g_device_hooks[i].name) == 0 && g_device_hooks[i].ptr)
            return (PFN_vkVoidFunction)g_device_hooks[i].ptr;
    }

    /* Forward to next layer */
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

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL idk_GetDeviceProcAddr(
    VkDevice device,
    const char *pName)
{
    /* Check our device hooks */
    for (size_t i = 0; i < sizeof(g_device_hooks) / sizeof(g_device_hooks[0]); i++) {
        if (strcmp(pName, g_device_hooks[i].name) == 0 && g_device_hooks[i].ptr)
            return (PFN_vkVoidFunction)g_device_hooks[i].ptr;
    }

    /* Forward to next layer */
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

/* ── Layer negotiation entry point ──────────────────────────────────────
 * The Vulkan loader calls this function when loading the layer.
 * We return our GetInstanceProcAddr and GetDeviceProcAddr. */

VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
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
