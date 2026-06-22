/*
 * idk_gl.h — OpenGL hook + SHM fallback interface
 *
 * When injected via syringe, this hooks glXSwapBuffers and eglSwapBuffers.
 * For OpenGL, dmabuf export of GLX/EGL renderbuffers is unreliable, so we
 * fall back to reading pixels into /dev/shm (shared memory).
 */
#ifndef IDK_GL_H
#define IDK_GL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── GL hook initialization ────────────────────────────────────────────── */

/**
 * Initialize OpenGL hooks.
 *
 * Installs hooks on:
 *   - glXSwapBuffers     (GLX / X11)
 *   - eglSwapBuffers     (EGL / Wayland or embedded)
 *   - SDL_GL_SwapWindow  (SDL2/SDL3 apps)
 *
 * When the target swaps buffers, this reads pixel data and sends it over
 * the IPC socket via /dev/shm (SHM fallback).
 *
 * @param ipc_fd         Connected IPC socket fd.
 * @param shm_base       Base path for SHM files (default: "/dev/shm/idk-overlay-XXXXXX").
 * @return               0 on success, -1 on failure.
 *
 * NOTE: SHM readback is slower than dmabuf but works universally.
 *       For Vulkan apps, use idk_vulkan_init() instead.
 */
int idk_gl_init(int ipc_fd, const char *shm_base);

/**
 * Shutdown OpenGL hooks.
 */
void idk_gl_shutdown(void);

/**
 * Initialize GL shaders/VBO for compositor.
 * Must be called from a GL context (after target process has created one).
 * @return 0 on success, -1 on failure.
 */
int idk_gl_init_gl_resources(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_GL_H */
