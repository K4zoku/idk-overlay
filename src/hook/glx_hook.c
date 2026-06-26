/* glx_hook.c — GLX swap hook for overlay compositing. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

#include "hook/syringe_hook.h"
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
static pthread_mutex_t g_hook_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&g_hook_mutex);
    if (g_hook_installed) {
        pthread_mutex_unlock(&g_hook_mutex);
        return 0;
    }

    /* Skip install_addr inline trampoline — it patches function code
     * directly and races with the game's main loop calling glXSwapBuffers,
     * causing SIGSEGV at unmapped addresses near xshmfence regions.
     * Let syringe_hook_install do GOT walk first (no code patching),
     * which redirects callers via GOT entries before any inline patch
     * is attempted. */
    int n = syringe_hook_install("glXSwapBuffers",
                                  (void *)glXSwapBuffers,
                                  (void **)&orig_glXSwapBuffers);
    if (n > 0) {
        g_hook_installed = 1;
        IDK_LOG("glx", "syringe hook installed (GOT)\n");
        pthread_mutex_unlock(&g_hook_mutex);
        return 0;
    }

    orig_glXSwapBuffers = (GlXSwapBuffersFn)hook_orig("glXSwapBuffers");
    if (orig_glXSwapBuffers && orig_glXSwapBuffers != (void *)glXSwapBuffers) {
        g_hook_installed = 1;
        IDK_LOG("glx", "LD_PRELOAD mode\n");
        pthread_mutex_unlock(&g_hook_mutex);
        return 0;
    }

    IDK_LOG("glx", "hook install failed\n");
    pthread_mutex_unlock(&g_hook_mutex);
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
