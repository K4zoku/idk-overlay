/*
 * idk_egl.h — EGL hook interface for ptrace inject path
 *
 * Used by libidk-overlay.so when injected into a process that uses EGL
 * (e.g. osu-lazer on Wayland with OpenGL renderer).
 *
 * Unlike the LD_PRELOAD path (idk_shim.c + idk_gl_hook.c), this path
 * hooks eglSwapBuffers directly in libEGL.so code via
 * syringe_hook_install_addr() — bypassing the GOT walk that fails
 * when SDL dlopen's libEGL.so.1 itself.
 *
 * Architecture:
 *   libidk-overlay.so injected into target process
 *     → constructor calls idk_egl_init()
 *     → idk_egl_init() dlopen libEGL.so.1
 *     → dlsym eglSwapBuffers
 *     → syringe_hook_install_addr("eglSwapBuffers", addr, hook, &orig)
 *     → hook_eglSwapBuffers receives overlay frame from webview compositor
 *       and renders fullscreen quad BEFORE calling orig_eglSwapBuffers
 */
#ifndef IDK_EGL_H
#define IDK_EGL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize EGL hooks for ptrace inject path.
 *
 * Resolves eglSwapBuffers from libEGL.so.1 and installs an inline
 * trampoline via syringe_hook_install_addr(). Also starts the
 * compositor (Unix socket server) to receive DMA-BUF frames from
 * the Qt webview client.
 *
 * @return 0 on success, -1 on failure.
 */
int idk_egl_init(void);

/**
 * Shutdown EGL hooks.
 * Removes the trampoline and closes the compositor.
 */
void idk_egl_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_EGL_H */
