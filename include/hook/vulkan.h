/*
 * idk_vulkan.h — Vulkan hook + dmabuf export interface
 *
 * When injected via syringe, this captures VkImage from vkQueuePresentKHR
 * and exports it as a dmabuf fd for zero-copy transport.
 */
#ifndef IDK_VULKAN_H
#define IDK_VULKAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── DRM FourCC formats (from linux/drm_fourcc.h) ──────────────────────── */

#define IDK_FORMAT_ABGR8888  0x34325258  /* DRM_FORMAT_ABGR8888 */
#define IDK_FORMAT_BGRA8888  0x56314742  /* DRM_FORMAT_BGRA8888  */
#define IDK_FORMAT_XBGR8888  0x32315258  /* DRM_FORMAT_XBGR8888  */

/* ── Vulkan initialization ─────────────────────────────────────────────── */

/**
 * Initialize Vulkan hooking.
 *
 * Installs a hook on vkQueuePresentKHR via syringe_hook_install().
 * When the target calls vkQueuePresentKHR, this exports the swapchain
 * image as a dmabuf fd and sends it over the IPC socket.
 *
 * @param ipc_fd         Connected IPC socket fd.
 * @param socket_path    Path used for IPC (for logging).
 * @return               0 on success, -1 on failure.
 *
 * NOTE: This function must be called AFTER the IPC socket is connected
 *       (idk_ipc_connect). The Vulkan hooks will only work if the target
 *       process links against libvulkan.so or a Vulkan ICD loader.
 */
int idk_vulkan_init(int ipc_fd, const char *socket_path);

/**
 * Shutdown Vulkan hooks.
 * Removes the installed hooks.
 */
void idk_vulkan_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_VULKAN_H */
