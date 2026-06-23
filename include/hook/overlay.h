/*
 * idk_overlay.h — Public interface for the injectable frame-capture library
 *
 * Usage:
 *   This header is included by the injected .so. The .so's __attribute__((constructor))
 *   calls idk_overlay_init() which installs GOT hooks and starts the IPC pipe.
 */
#ifndef IDK_OVERLAY_H
#define IDK_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Frame metadata (sent alongside dmabuf fd via Unix socket) ─────────── */

typedef struct idk_frame_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;       /* pixels per row in the dmabuf (may differ from width) */
    uint32_t format;       /* DRM_FORMAT_ABGR8888 / ARGB8888 / etc. (linux/drm_fourcc.h) */
    uint32_t num_planes;   /* 1 for single-plane formats */
    uint32_t pid;          /* capturing process PID (for debugging) */
} idk_frame_info_t;

/* ── Initialization ────────────────────────────────────────────────────── */

/**
 * Initialize the overlay capture system.
 *
 * Called from __attribute__((constructor)) — installs hooks automatically
 * via syringe_hook_install() for the graphics stack the target uses.
 *
 * @param socket_path  Unix socket path for IPC (e.g., "/tmp/idk-overlay-1234").
 *                     If NULL, defaults to "/tmp/idk-overlay".
 * @param enable_vk    Non-zero to hook Vulkan (vkQueuePresentKHR).
 * @param enable_gl    Non-zero to hook OpenGL (glXSwapBuffers, eglSwapBuffers).
 * @return             0 on success, -1 on failure.
 */
int idk_overlay_init(const char *socket_path, int enable_vk, int enable_gl);

/**
 * Tear down hooks and close IPC.
 * Called from __attribute__((destructor)).
 */
void idk_overlay_shutdown(void);

/**
 * Try to install the Wayland input hook (idk_wayland_input_init).
 * Idempotent and safe to call from any thread — internally guarded by
 * a once-flag. The hook only installs if libwayland-client.so.0 is
 * loaded in the process. Designed to be called from the EGL/GLX/Vulkan
 * swap hook on first swap (after the graphics hook is confirmed working
 * and libwayland-client is guaranteed loaded).
 *
 * @return 0 on success or already-installed, -1 if wayland not available.
 */
int idk_overlay_try_install_wayland_input(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_OVERLAY_H */
