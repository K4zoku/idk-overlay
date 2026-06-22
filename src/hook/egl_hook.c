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
#include "shim/elfhacks.h"

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
static int g_hook_installed = 0;
static int g_retry_done = 0;
static int g_gl_resources_ready = 0;

static void *(*real_dlsym)(void *, const char *) = NULL;
static void *(*real_dlopen)(const char *, int) = NULL;
static int (*real_dlclose)(void *) = NULL;

static void load_real_functions(void) {
    if (real_dlsym) return;

    eh_obj_t lib;
    const char *libs[] = {
#if defined(__GLIBC__)
        "*libdl.so*",
#endif
        "*libc.so*",
        "*libc.*.so*",
        "*ld-musl-*.so*",
    };

    for (size_t i = 0; i < sizeof(libs) / sizeof(*libs); i++) {
        if (eh_find_obj(&lib, libs[i]) != 0) continue;
        eh_find_sym(&lib, "dlsym", (void **)&real_dlsym);
        eh_find_sym(&lib, "dlopen", (void **)&real_dlopen);
        eh_find_sym(&lib, "dlclose", (void **)&real_dlclose);
        eh_destroy_obj(&lib);
        if (real_dlsym) break;
    }

    if (real_dlsym) {
        IDK_LOG("egl", "real functions loaded: dlsym=%p dlopen=%p\n",
                (void *)real_dlsym, (void *)real_dlopen);
    }
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);

static EGLBoolean call_real_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    load_real_functions();
    if (real_dlsym) {
        void *lib = dlopen("libEGL.so.1", RTLD_LAZY);
        if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY);
        if (lib) {
            EGLBoolean (*fn)(EGLDisplay, EGLSurface) =
                (EGLBoolean (*)(EGLDisplay, EGLSurface))real_dlsym(lib, "eglSwapBuffers");
            if (fn) {
                EGLBoolean ret = fn(dpy, surface);
                if (real_dlclose) real_dlclose(lib);
                return ret;
            }
            if (real_dlclose) real_dlclose(lib);
        }
    }
    return 0;
}

static int install_egl_hook(void) {
    load_real_functions();
    if (g_hook_installed) return 0;
    g_hook_installed = 1;

    void *egl_handle = NULL;
    void *egl_swap_addr = NULL;
    int n = 0;

    if (real_dlopen) {
        egl_handle = real_dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!egl_handle)
            egl_handle = real_dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!egl_handle)
            egl_handle = real_dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    } else {
        egl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!egl_handle)
            egl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!egl_handle)
            egl_handle = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    }

    if (egl_handle) {
        if (real_dlsym)
            egl_swap_addr = real_dlsym(egl_handle, "eglSwapBuffers");
        if (!egl_swap_addr)
            egl_swap_addr = dlsym(egl_handle, "eglSwapBuffers");
    }

    if (idk_compositor_init() != 0) {
        EDBG("compositor init failed");
    }

    if (egl_swap_addr) {
        n = syringe_hook_install_addr("eglSwapBuffers",
                                      egl_swap_addr,
                                      (void *)hook_eglSwapBuffers,
                                      (void **)&orig_eglSwapBuffers);
        if (n > 0) {
            EDBG("installed via install_addr");
            return 0;
        }
    }

    n = syringe_hook_install("eglSwapBuffers",
                             (void *)hook_eglSwapBuffers,
                             (void **)&orig_eglSwapBuffers);
    if (n > 0) {
        EDBG("installed via GOT walk");
        return 0;
    }

    if (egl_swap_addr) {
        n = syringe_hook_install_addr("eglSwapBuffers",
                                      egl_swap_addr,
                                      (void *)hook_eglSwapBuffers,
                                      (void **)&orig_eglSwapBuffers);
        if (n > 0) {
            EDBG("installed via install_addr (retry)");
            return 0;
        }
    }

    EDBG("all install methods failed, deferring to first call");
    return -1;
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!orig_eglSwapBuffers && !g_retry_done) {
        g_retry_done = 1;
        install_egl_hook();
    }

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
    return call_real_eglSwapBuffers(dpy, surface);
}

int idk_egl_init(void) {
    if (g_hook_installed) return 0;
    return install_egl_hook();
}

void idk_egl_shutdown(void) {
    if (!g_hook_installed) return;
    syringe_hook_remove("eglSwapBuffers");
    idk_compositor_shutdown();
    g_hook_installed = 0;
    g_gl_resources_ready = 0;
    g_retry_done = 0;
}
