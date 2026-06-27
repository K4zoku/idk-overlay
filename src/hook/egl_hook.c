#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#include "hook/syringe_hook.h"
#include "hook/graphic_hooks.h"
#include "hook/hook_util.h"
#include "hook/hook_plugin.h"
#include "hook/wayland_input.h"
#include "gl/gl_loader.h"
#include "core/compositor_egl.h"
#include "core/log.h"

extern _Atomic int g_overlay_visible;

typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;
#define EGL_WIDTH  0x3057
#define EGL_HEIGHT 0x3056

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;
static int g_hook_installed = 0;
static int g_gl_resources_ready = 0;
static pthread_mutex_t g_hook_mutex = PTHREAD_MUTEX_INITIALIZER;

static EGLBoolean (*fn_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint*) = NULL;

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!orig_eglSwapBuffers)
        orig_eglSwapBuffers = (EGLBoolean (*)(EGLDisplay, EGLSurface))hook_orig("eglSwapBuffers");

    idk_wayland_input_sidecar_dispatch();

    if (!g_gl_resources_ready) {
        if (idk_compositor_egl_init_gl() == 0)
            g_gl_resources_ready = 1;
    }

    idk_compositor_egl_render();

    if (!fn_eglQuerySurface)
        fn_eglQuerySurface = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*))
            dlsym(RTLD_DEFAULT, "eglQuerySurface");
    if (fn_eglQuerySurface && dpy && surface) {
        EGLint surf_w = 0, surf_h = 0;
        if (fn_eglQuerySurface(dpy, surface, EGL_WIDTH, &surf_w) &&
            fn_eglQuerySurface(dpy, surface, EGL_HEIGHT, &surf_h))
            idk_compositor_egl_notify_resize((int)surf_w, (int)surf_h);
    }

    if (g_overlay_visible && idk_compositor_egl_has_overlay() && idk_fn_glGetIntegerv) {
        GLint vp[4] = {0, 0, 0, 0};
        idk_fn_glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] > 0 && vp[3] > 0)
            idk_compositor_egl_render_overlay(0, 0, (uint32_t)vp[2], (uint32_t)vp[3]);
    }

    return orig_eglSwapBuffers(dpy, surface);
}

static int install_egl_hook(void) {
    pthread_mutex_lock(&g_hook_mutex);
    if (g_hook_installed) {
        pthread_mutex_unlock(&g_hook_mutex);
        return 0;
    }

    if (idk_compositor_egl_init() != 0)
        IDK_LOG("egl", "compositor init queued (no GL context yet)\n");

    /* Step 1: Resolve eglSwapBuffers address from libEGL.so.
     * Needed for syringe_hook_install_addr (inline trampoline patching)
     * which works even when the game calls eglSwapBuffers via dlsym
     * (e.g. .NET runtime, SDL3 dlopen'd EGL). */
    void *egl_swap_addr = NULL;
    void *lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib) egl_swap_addr = dlsym(lib, "eglSwapBuffers");

    /* Step 2: Try syringe_hook_install_addr (inline trampoline) first.
     * This patches the actual function code, catching ALL callers
     * including dlsym-based ones. */
    if (egl_swap_addr) {
        int n = syringe_hook_install_addr("eglSwapBuffers",
                                           egl_swap_addr,
                                           (void *)eglSwapBuffers,
                                           (void **)&orig_eglSwapBuffers);
        if (n > 0) {
            g_hook_installed = 1;
            IDK_LOG("egl", "hook installed via install_addr (trampoline)\n");
            pthread_mutex_unlock(&g_hook_mutex);
            return 0;
        }
    }

    /* Step 3: Try syringe GOT/PLT walk. */
    int n = syringe_hook_install("eglSwapBuffers",
                                  (void *)eglSwapBuffers,
                                  (void **)&orig_eglSwapBuffers);
    if (n > 0) {
        g_hook_installed = 1;
        IDK_LOG("egl", "syringe hook installed (GOT)\n");
        pthread_mutex_unlock(&g_hook_mutex);
        return 0;
    }

    /* Step 4: LD_PRELOAD fallback - RTLD_NEXT resolves the real function. */
    orig_eglSwapBuffers = (EGLBoolean (*)(EGLDisplay, EGLSurface))
        hook_orig("eglSwapBuffers");
    if (orig_eglSwapBuffers && orig_eglSwapBuffers != (void *)eglSwapBuffers) {
        g_hook_installed = 1;
        IDK_LOG("egl", "LD_PRELOAD mode (syringe not needed)\n");
        pthread_mutex_unlock(&g_hook_mutex);
        return 0;
    }

    IDK_LOG("egl", "hook install failed\n");
    pthread_mutex_unlock(&g_hook_mutex);
    return -1;
}

int idk_egl_init(void) {
    if (g_hook_installed) return 0;
    return install_egl_hook();
}

void idk_egl_shutdown(void) {
    if (!g_hook_installed) return;
    syringe_hook_remove("eglSwapBuffers");
    g_hook_installed = 0;
    g_gl_resources_ready = 0;
}

idk_hook_plugin_t idk_plugin_egl = {
    .name = "egl",
    .lib_patterns = {"libEGL.so.1", "libEGL.so", NULL},
    .init  = idk_egl_init,
    .shutdown = idk_egl_shutdown,
};
