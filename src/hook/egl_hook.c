#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>

#include "hook/syringe_hook.h"
#include "hook/egl.h"
#include "gl/gl_loader.h"
#include "core/compositor.h"
#include "core/log.h"

static FILE *g_dbg = NULL;
static void egl_dbg_init(void) {
    if (g_dbg) return;
    g_dbg = fopen("/tmp/idk-debug.log", "a");
    if (g_dbg) setvbuf(g_dbg, NULL, _IONBF, 0);
}
#define EDBG(fmt, ...) do { \
    egl_dbg_init(); \
    if (g_dbg) fprintf(g_dbg, "[idk-egl] " fmt "\n", ##__VA_ARGS__); \
    IDK_LOG("egl", fmt "\n", ##__VA_ARGS__); \
} while(0)

typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef unsigned int EGLBoolean;

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;

static int g_initialized = 0;
static int g_gl_resources_ready = 0;

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!g_gl_resources_ready) {
        if (idk_compositor_init_gl() == 0) {
            g_gl_resources_ready = 1;
        } else {
            EDBG("GL resources init failed");
        }
    }

    idk_compositor_render();

    if (idk_compositor_has_overlay() && idk_fn_glGetIntegerv) {
        GLint vp[4] = {0, 0, 0, 0};
        idk_fn_glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] > 0 && vp[3] > 0) {
            idk_compositor_render_overlay(0, 0, (uint32_t)vp[2], (uint32_t)vp[3]);
        }
    }

    if (orig_eglSwapBuffers) {
        return orig_eglSwapBuffers(dpy, surface);
    }
    return 0;
}

int idk_egl_init(void) {
    if (g_initialized) return 0;
    g_initialized = 1;

    EDBG("init");

    /* 
     * SDL dlopen's libEGL.so.1 with RTLD_LOCAL, so dlsym(RTLD_DEFAULT)
     * won't find eglSwapBuffers. Use dlopen(RTLD_NOLOAD) to grab the
     * already-loaded handle, then dlsym on that handle.
     */
    void *egl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!egl_handle) {
        egl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!egl_handle)
            egl_handle = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!egl_handle) {
        EDBG("dlopen libEGL.so.1 failed: %s", dlerror());
        return -1;
    }

    void *egl_swap_addr = dlsym(egl_handle, "eglSwapBuffers");
    if (!egl_swap_addr) {
        EDBG("dlsym eglSwapBuffers failed: %s", dlerror());
        return -1;
    }

    if (idk_compositor_init() != 0) {
        EDBG("compositor init failed");
    }

    /*
     * Use install_addr to bypass GOT walk. The GOT walk (syringe_hook_install)
     * fails when the target dlopen'd libEGL privately (SDL pattern).
     * install_addr patches the function code directly.
     */
    int n = syringe_hook_install_addr("eglSwapBuffers",
                                       egl_swap_addr,
                                       (void*)hook_eglSwapBuffers,
                                       (void**)&orig_eglSwapBuffers);
    if (n <= 0) {
        EDBG("syringe_hook_install_addr failed");
        return -1;
    }

    return 0;
}

void idk_egl_shutdown(void) {
    if (!g_initialized) return;
    syringe_hook_remove("eglSwapBuffers");
    idk_compositor_shutdown();
    g_initialized = 0;
    g_gl_resources_ready = 0;
}
