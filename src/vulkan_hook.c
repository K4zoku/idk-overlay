/*
 * vulkan_hook.c — Vulkan hook + dmabuf export
 *
 * Hooks vkQueuePresentKHR via syringe_hook_install().
 * When the target presents a frame, we:
 *   1. Find the VkSwapchainKHR from the pPresentInfo
 *   2. Export the VkImage as dmabuf fd (VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
 *   3. Send the fd + metadata over IPC
 *
 * Requires:
 *   - libvulkan.so loaded in the target process
 *   - Vulkan 1.2+ (for dma_buf support) or VK_KHR_external_memory_fd extension
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>

#include "hook/syringe_hook.h"
#include "idk_vulkan.h"
#include "idk_ipc.h"

/* ── Vulkan types (opaque — we don't need full headers) ────────────────── */

typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef void* VkQueue;
typedef void* VkSwapchainKHR;
typedef void* VkImage;
typedef void* VkSemaphore;
typedef uint32_t VkResult;
typedef uint32_t VkFlags;
typedef uint32_t VkStructureType;

#define VK_SUCCESS 0
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

/* ── Internal state ────────────────────────────────────────────────────── */

static int g_ipc_fd = -1;
static const char *g_sockpath = NULL;

/* Hook function pointer types */
typedef VkResult (*PFN_vkQueuePresentKHR)(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);

/* We store the original function pointer here */
static PFN_vkQueuePresentKHR orig_QueuePresentKHR = NULL;
static void *orig_QueuePresentKHR_store = NULL;

/* ── Vulkan proc address resolution ────────────────────────────────────── */

static void *resolve_vk_proc(const char *name) {
    void *handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!handle) {
        handle = dlopen("libvulkan.so.1.3.290", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!handle) {
        fprintf(stderr, "[idk-vk] dlopen libvulkan failed: %s\n", dlerror());
        return NULL;
    }

    void *sym = dlsym(handle, name);
    if (sym) {
        dlclose(handle);
    }
    return sym;
}

/* ── DMABUF export helper ──────────────────────────────────────────────── */

/*
 * Minimal dmabuf export from VkImage via vkExportMemoryFdKHR.
 *
 * In a full implementation, we would:
 *   1. Get the VkImage handle from the swapchain
 *   2. Create a VkExportMemoryFdInfoKHR with handleType = DMA_BUF
 *   3. Call vkExportMemoryFdKHR to get the dmabuf fd
 *   4. Send the fd over IPC
 *
 * For now, we stub this — the actual implementation requires full Vulkan
 * layer setup (instance/device dispatch tables) which is complex to hook
 * via GOT alone. A simpler approach is to read the frame via
 * vkCmdReadBufferBitsEXT (VK_EXT_image_drm_image_format) if available,
 * or fall back to reading the present queue's image directly.
 */

static int export_image_to_dmabuf(VkImage image, uint32_t width,
                                  uint32_t height, uint32_t *out_fd) {
    /*
     * TODO: Full implementation requires:
     *   - Finding the VkImage from the swapchain via vkGetSwapchainImagesKHR
     *   - Getting device dispatch table for vkExportMemoryFdKHR
     *   - Setting up VkExportMemoryFdInfoKHR with DMA_BUF handle type
     *   - Calling vkExportMemoryFdKHR to get dmabuf fd
     *
     * This is a PLACEHOLDER — dmabuf export from Vulkan requires deep
     * layer integration. For a quick prototype, consider:
     *   - Using VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT via
     *     VK_KHR_external_memory_fd extension
     *   - Or: reading the present image via vkCmdPipelineBarrier + vkMapMemory
     *     (slower but works without full layer setup)
     */
    (void)image;
    (void)width;
    (void)height;
    (void)out_fd;

    /* Stub: return -1 to indicate "not yet implemented" */
    return -1;
}

/* ── Hook implementation ───────────────────────────────────────────────── */

static VkResult hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (orig_QueuePresentKHR && pPresentInfo) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            /*
             * TODO: Extract VkImage from swapchain at index pPresentInfo->pImageIndices[i]
             * For now, we can't easily get the VkImage without full layer setup.
             *
             * Alternative approach: hook vkCreateSwapchainKHR to track swapchain
             * metadata (width, height, VkImage array), then lookup the image
             * by swapchain handle and image index here.
             */
            fprintf(stderr, "[idk-vk] QueuePresentKHR swapchain[%u] (stub)\n", i);
            /*
            VkSwapchainKHR sc = pPresentInfo->pSwapchains[i];
            uint32_t img_idx = pPresentInfo->pImageIndices[i];

            // Look up swapchain data (requires hooking vkCreateSwapchainKHR)
            swapchain_meta *meta = lookup_swapchain(sc);
            if (meta) {
                VkImage img = meta->images[img_idx];
                int dmabuf_fd = export_image_to_dmabuf(img, meta->width, meta->height, NULL);
                if (dmabuf_fd >= 0) {
                    idk_frame_info_t info = {
                        .width = meta->width,
                        .height = meta->height,
                        .stride = meta->width,
                        .format = IDK_FORMAT_ABGR8888,
                        .num_planes = 1,
                        .pid = getpid(),
                    };
                    idk_ipc_send_frame(g_ipc_fd, &info, sizeof(info), dmabuf_fd);
                    close(dmabuf_fd);
                }
            }
            */
        }
    }

    /* Call original function */
    if (orig_QueuePresentKHR) {
        return orig_QueuePresentKHR(queue, pPresentInfo);
    }
    return VK_SUCCESS;
}

/* ── Public API ────────────────────────────────────────────────────────── */

int idk_vulkan_init(int ipc_fd, const char *socket_path) {
    g_ipc_fd = ipc_fd;
    g_sockpath = socket_path;

    /* Try to resolve and hook vkQueuePresentKHR */
    void *sym = resolve_vk_proc("vkQueuePresentKHR");
    if (!sym) {
        fprintf(stderr, "[idk-vk] vkQueuePresentKHR not found — Vulkan hook skipped\n");
        return -1;
    }

    fprintf(stderr, "[idk-vk] vkQueuePresentKHR resolved at %p\n", sym);

    int n = syringe_hook_install("vkQueuePresentKHR", (void *)hook_QueuePresentKHR,
                                  &orig_QueuePresentKHR_store);

    if (n > 0) {
        /* GOT slots patched + trampoline installed */
        orig_QueuePresentKHR = (PFN_vkQueuePresentKHR)orig_QueuePresentKHR_store;
        fprintf(stderr, "[idk-vk] Hook installed (%d GOT patches, trampoline)\n", n);
    } else {
        /* No GOT entries and dlsym failed. Install trampoline directly
         * at the dlopen-resolved address. This handles Vulkan symbols
         * not in RTLD_DEFAULT global scope. */
        fprintf(stderr, "[idk-vk] syringe_hook_install failed — building direct trampoline at %p\n", sym);
        fprintf(stderr, "[idk-vk] About to mmap...\n");
        void *trampoline = mmap(NULL, 64, PROT_READ|PROT_WRITE|PROT_EXEC,
                                MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        fprintf(stderr, "[idk-vk] mmap returned %p\n", trampoline);
        if (trampoline == MAP_FAILED) {
            fprintf(stderr, "[idk-vk] mmap trampoline failed\n");
            orig_QueuePresentKHR = (PFN_vkQueuePresentKHR)sym;
        } else {
            fprintf(stderr, "[idk-vk] mmap OK, building jump...\n");
            /* Build: jmp *hook (FF 25 00 00 00 00 + absolute addr) */
            uint8_t *jmp = (uint8_t*)trampoline;
            jmp[0] = 0xFF; jmp[1] = 0x25; jmp[2] = jmp[3] = jmp[4] = jmp[5] = 0;
            memcpy(jmp + 6, &hook_QueuePresentKHR, 8);
            fprintf(stderr, "[idk-vk] jump built OK\n");

            /* Build reverse jump: jmp <original+14> */
            uint8_t *ret = (uint8_t*)trampoline + 14;
            uint8_t rev_jmp[14] = { 0 };
            rev_jmp[0] = 0xFF; rev_jmp[1] = 0x25;
            uint8_t *ret_site = (uint8_t*)sym + 14;
            memcpy(rev_jmp + 6, &ret_site, 8);

            /* Save stolen bytes using /proc/self/mem (safe for non-readable pages) */
            uint8_t stolen[14];
            if (syringe_hook_mem_read(stolen, sym, 14) != 0) {
                fprintf(stderr, "[idk-vk] Could not read stolen bytes via /proc/self/mem\n");
                munmap(trampoline, 64);
                orig_QueuePresentKHR = (PFN_vkQueuePresentKHR)sym;
                return 0; /* bail — can't steal prologue */
            }

            /* Write reverse jump at original (need to patch read-only code) */
            fprintf(stderr, "[idk-vk] Calling syringe_hook_safe_write...\n");
            if (syringe_hook_safe_write(sym, rev_jmp, 14) == 0) {
                __builtin___clear_cache((char*)sym, (char*)sym + 14);
                orig_QueuePresentKHR = (PFN_vkQueuePresentKHR)trampoline;
                /* Store restore data in the hook record */
                if (orig_QueuePresentKHR_store) {
                    memcpy(orig_QueuePresentKHR_store, stolen, 14);
                }
                fprintf(stderr, "[idk-vk] Direct trampoline installed @ %p → %p\n",
                        sym, trampoline);
            } else {
                fprintf(stderr, "[idk-vk] safe_write failed — hook NOT installed\n");
                munmap(trampoline, 64);
                orig_QueuePresentKHR = (PFN_vkQueuePresentKHR)sym;
            }
        }
    }

    return 0;
}

void idk_vulkan_shutdown(void) {
    syringe_hook_remove("vkQueuePresentKHR");
    orig_QueuePresentKHR = NULL;
    orig_QueuePresentKHR_store = NULL;
}
