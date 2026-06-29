/* GLX swap hook for overlay compositing. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

#include "hook/syringe_hook.h"
#include "hook/hook_util.h"
#include "hook/hook_plugin.h"
#include "core/compositor.h"
#include "core/compositor_egl.h"
#include "core/log.h"
#include "gl/gl_loader.h"

extern _Atomic int g_overlay_visible;

typedef void* Display;
typedef void* GLXDrawable;
typedef void (*GlXSwapBuffersFn)(Display *, GLXDrawable);

static GlXSwapBuffersFn orig_glXSwapBuffers = NULL;
static int g_hook_installed = 0;
static int g_gl_resources_ready = 0;
static pthread_mutex_t g_hook_mutex = PTHREAD_MUTEX_INITIALIZER;

void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    if (!orig_glXSwapBuffers)
        orig_glXSwapBuffers = (GlXSwapBuffersFn)hook_orig("glXSwapBuffers");
    if (!orig_glXSwapBuffers ||
        orig_glXSwapBuffers == (GlXSwapBuffersFn)(void *)glXSwapBuffers)
        return;

    if (!g_gl_resources_ready) {
        if (idk_compositor_egl_init_gl() == 0)
            g_gl_resources_ready = 1;
    }

    idk_compositor_egl_render();

    if (g_overlay_visible && idk_compositor_egl_has_overlay() && idk_fn_glGetIntegerv) {
        GLint vp[4] = {0, 0, 0, 0};
        idk_fn_glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] > 0 && vp[3] > 0) {
            idk_compositor_egl_notify_resize((int)vp[2], (int)vp[3]);
            idk_compositor_egl_render_overlay(0, 0, (uint32_t)vp[2], (uint32_t)vp[3]);
        }
    }

    orig_glXSwapBuffers(dpy, drawable);
}

static GlXSwapBuffersFn resolve_real_glXSwapBuffers(void) {
    void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libGL.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libGL.so", RTLD_NOW);
    if (!lib) return NULL;
    return (GlXSwapBuffersFn)dlsym(lib, "glXSwapBuffers");
}

static int install_glx_hook(void) {
    pthread_mutex_lock(&g_hook_mutex);
    if (g_hook_installed) {
        pthread_mutex_unlock(&g_hook_mutex);
        return 0;
    }

    idk_compositor_init();

    GlXSwapBuffersFn real_fn = resolve_real_glXSwapBuffers();

    /* Step 1: syringe GOT walk. Patches GOT entries pointing to
     * glXSwapBuffers so callers reach our hook. Thread-safe (no code
     * patching).
     *
     * BUG: syringe resolves orig via dlsym(RTLD_DEFAULT, ...) which,
     * under LD_PRELOAD, returns OUR hook - not libGL's real function.
     * syringe then sets *orig_out = our_hook_address (self-reference).
     * The runtime `if (orig == self) return;` would catch this but
     * turn glXSwapBuffers into a no-op (gears freeze, 38K FPS reported,
     * no actual swap). Override orig with the real address instead.
     * Safe because syringe skips the inline trampoline when orig==hook,
     * so the real function is unpatched - we can call it directly. */
    int n = syringe_hook_install("glXSwapBuffers",
                                  (void *)glXSwapBuffers,
                                  (void **)&orig_glXSwapBuffers);
    if (n > 0) {
        if (orig_glXSwapBuffers == (void *)glXSwapBuffers) {
            IDK_LOG("glx", "syringe returned self-referencing orig, overriding with libGL addr=%p\n",
                    (void *)real_fn);
            orig_glXSwapBuffers = real_fn;
        }
        if (orig_glXSwapBuffers && orig_glXSwapBuffers != (void *)glXSwapBuffers) {
            g_hook_installed = 1;
            IDK_LOG("glx", "syringe hook installed (GOT)\n");
            pthread_mutex_unlock(&g_hook_mutex);
            return 0;
        }
    }

    /* Step 2: LD_PRELOAD fallback. RTLD_NEXT skips our LD_PRELOAD'd
     * definition and returns libGL's real glXSwapBuffers. */
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
