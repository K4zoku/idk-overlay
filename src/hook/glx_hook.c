/* glx_hook.c — GLX swap hook for overlay compositing.
 *
 * Mirrors the EGL hook: hooks glXSwapBuffers, inits compositor,
 * per-swap calls idk_compositor_egl_render() (receive overlay frame from
 * webview) + idk_compositor_egl_render_overlay() (draw overlay on top).
 *
 * The compositor code (src/core/compositor_egl.c) uses gl/gl_loader.h
 * function pointers which work under any current GL context — GLX
 * or EGL. No GLX-specific GL code needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "hook/syringe_hook.h"
#include "hook/graphic_hooks.h"
#include "hook/hook_util.h"
#include "hook/hook_plugin.h"
#include "core/compositor_egl.h"
#include "core/log.h"
#include "gl/gl_loader.h"

typedef void* Display;
typedef void* GLXDrawable;
typedef void (*GlXSwapBuffersFn)(Display *, GLXDrawable);

static GlXSwapBuffersFn orig_glXSwapBuffers = NULL;
static int g_hook_installed = 0;
static int g_compositor_inited = 0;
static int g_gl_resources_ready = 0;

void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    if (!orig_glXSwapBuffers)
        orig_glXSwapBuffers = (GlXSwapBuffersFn)hook_orig("glXSwapBuffers");

    if (!g_compositor_inited) {
        if (idk_compositor_egl_init() == 0)
            g_compositor_inited = 1;
    }

    if (g_compositor_inited && !g_gl_resources_ready) {
        if (idk_compositor_egl_init_gl() == 0)
            g_gl_resources_ready = 1;
    }

    idk_compositor_egl_render();

    if (idk_compositor_egl_has_overlay() && idk_fn_glGetIntegerv) {
        GLint vp[4] = {0, 0, 0, 0};
        idk_fn_glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] > 0 && vp[3] > 0) {
            idk_compositor_egl_notify_resize((int)vp[2], (int)vp[3]);
            idk_compositor_egl_render_overlay(0, 0, (uint32_t)vp[2], (uint32_t)vp[3]);
        }
    }

    orig_glXSwapBuffers(dpy, drawable);
}

static int install_glx_hook(void) {
    if (g_hook_installed) return 0;

    /* LD_PRELOAD: our symbol already wins linker resolution. */
    if (hook_is_ld_preload("glXSwapBuffers", (void *)glXSwapBuffers)) {
        IDK_LOG("glx", "LD_PRELOAD detected — no syringe hook needed\n");
        orig_glXSwapBuffers = (GlXSwapBuffersFn)hook_orig("glXSwapBuffers");
        g_hook_installed = 1;
        return 0;
    }

    /* Late inject: patch GOT/PLT via syringe */
    int n = syringe_hook_install("glXSwapBuffers",
                                  (void *)glXSwapBuffers,
                                  (void **)&orig_glXSwapBuffers);
    if (n > 0) {
        g_hook_installed = 1;
        IDK_LOG("glx", "syringe hook installed\n");
        return 0;
    }

    IDK_LOG("glx", "syringe hook install failed\n");
    return -1;
}

int idk_glx_init(void) {
    if (g_hook_installed) return 0;
    return install_glx_hook();
}

void idk_glx_shutdown(void) {
    if (!g_hook_installed) return;
    syringe_hook_remove("glXSwapBuffers");
    g_hook_installed = 0;
    g_gl_resources_ready = 0;
}

idk_hook_plugin_t idk_plugin_glx = {
    .name = "glx",
    .lib_patterns = {"libGL.so.1", "libGL.so", NULL},
    .init  = idk_glx_init,
    .shutdown = idk_glx_shutdown,
};
