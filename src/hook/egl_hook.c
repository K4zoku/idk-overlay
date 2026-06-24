#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>

#include "hook/syringe_hook.h"
#include "hook/egl.h"
#include "hook/overlay.h"
#include "hook/wayland_input.h"
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
typedef int32_t EGLint;
#define EGL_WIDTH  0x3057
#define EGL_HEIGHT 0x3056

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;
static int g_hook_installed = 0;
static int g_retry_done = 0;
static int g_wl_egl_retry_count = 0;
static int g_wl_egl_hook_installed = 0;
static int g_wl_egl_hook_gave_up = 0;
static int g_gl_resources_ready = 0;
static pthread_mutex_t g_hook_mutex = PTHREAD_MUTEX_INITIALIZER;

static EGLBoolean (*fn_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint*) = NULL;

typedef void (*PFN_glViewport_fn)(GLint, GLint, GLsizei, GLsizei);
static PFN_glViewport_fn orig_glViewport = NULL;
static int g_glViewport_hook_installed = 0;

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
static int install_wl_egl_hook(void);
static int ensure_eglQuerySurface(void);
static int install_glViewport_hook(void);

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
    pthread_mutex_lock(&g_hook_mutex);
    load_real_functions();
    if (g_hook_installed) {
        pthread_mutex_unlock(&g_hook_mutex);
        return 0;
    }

    void *egl_handle = NULL;
    void *egl_swap_addr = NULL;
    int n = 0;

    if (real_dlopen) {
        egl_handle = real_dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!egl_handle)
            egl_handle = real_dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    } else {
        egl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!egl_handle)
            egl_handle = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    }

    if (!egl_handle) {
        EDBG("libEGL not loaded yet — will retry");
        pthread_mutex_unlock(&g_hook_mutex);
        return -1;
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
            g_hook_installed = 1;
            EDBG("installed via install_addr");
            install_wl_egl_hook();
            pthread_mutex_unlock(&g_hook_mutex);
            return 0;
        }
    }

    n = syringe_hook_install("eglSwapBuffers",
                             (void *)hook_eglSwapBuffers,
                             (void **)&orig_eglSwapBuffers);
    if (n > 0) {
        g_hook_installed = 1;
        EDBG("installed via GOT walk");
        install_wl_egl_hook();
        pthread_mutex_unlock(&g_hook_mutex);
        return 0;
    }

    if (egl_swap_addr) {
        n = syringe_hook_install_addr("eglSwapBuffers",
                                       egl_swap_addr,
                                       (void *)hook_eglSwapBuffers,
                                       (void **)&orig_eglSwapBuffers);
        if (n > 0) {
            g_hook_installed = 1;
            EDBG("installed via install_addr (retry)");
            install_wl_egl_hook();
            pthread_mutex_unlock(&g_hook_mutex);
            return 0;
        }
    }

    EDBG("all install methods failed — will retry from background thread");
    pthread_mutex_unlock(&g_hook_mutex);
    return -1;
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!orig_eglSwapBuffers && !g_retry_done) {
        g_retry_done = 1;
        install_egl_hook();
    }
    if (!g_wl_egl_hook_installed && !g_wl_egl_hook_gave_up) {
        g_wl_egl_retry_count++;
        if (g_wl_egl_retry_count % 10 == 0) {
            int r = install_wl_egl_hook();
            if (r != 0 && g_wl_egl_retry_count >= 100) {
                g_wl_egl_hook_gave_up = 1;
                EDBG("wl_egl_window_resize gave up after %d retries", g_wl_egl_retry_count);
            }
        }
        if (!g_glViewport_hook_installed && g_wl_egl_retry_count == 30)
            install_glViewport_hook();
    }

    /* Install wayland input hook on first swap — by now libwayland-client
     * is guaranteed loaded (if this is a wayland client) and the EGL hook
     * is in place. Idempotent. */
    idk_overlay_try_install_wayland_input();

    idk_wayland_input_sidecar_dispatch();

    if (!g_gl_resources_ready) {
        if (idk_compositor_init_gl() == 0) {
            g_gl_resources_ready = 1;
        } else {
            EDBG("GL resources init failed");
        }
    }

    idk_compositor_render();

    /* Per-frame EGL surface size query — catches game resizes that skip
     * wl_egl_window_resize / vkCreateSwapchainKHR (e.g. SDL_SetWindowSize
     * on XWayland). Only triggers notify_resize on actual change. */
    if (!fn_eglQuerySurface) {
        if (ensure_eglQuerySurface() != 0) {
            EDBG("eglQuerySurface resolution failed");
        }
    }
    if (fn_eglQuerySurface && dpy && surface) {
        EGLint surf_w = 0, surf_h = 0;
        if (fn_eglQuerySurface(dpy, surface, EGL_WIDTH, &surf_w) &&
            fn_eglQuerySurface(dpy, surface, EGL_HEIGHT, &surf_h)) {
            idk_compositor_notify_resize((int)surf_w, (int)surf_h);
        }
    }

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

/* ── Resolve eglQuerySurface (try multiple methods) ────────────── */

static int ensure_eglQuerySurface(void) {
    if (fn_eglQuerySurface) return 0;

    void *lib = real_dlopen
        ? real_dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD)
        : dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = real_dlopen
        ? real_dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD)
        : dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib) {
        fn_eglQuerySurface = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*))
            (real_dlsym ? real_dlsym(lib, "eglQuerySurface") : dlsym(lib, "eglQuerySurface"));
        if (fn_eglQuerySurface) {
            EDBG("eglQuerySurface resolved from libEGL");
            return 0;
        }
    }

    fn_eglQuerySurface = dlsym(RTLD_DEFAULT, "eglQuerySurface");
    if (fn_eglQuerySurface) {
        EDBG("eglQuerySurface resolved via RTLD_DEFAULT");
        return 0;
    }

    return -1;
}

/* ── glViewport hook (resize signal) ──────────────────────────── */

static void hook_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (orig_glViewport)
        orig_glViewport(x, y, w, h);
    if (w > 0 && h > 0)
        idk_compositor_notify_resize((int)w, (int)h);
}

static int install_glViewport_hook(void) {
    if (g_glViewport_hook_installed) return 0;

    void *sym = NULL;
    void *libgl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!libgl) libgl = dlopen("libGL.so.1", RTLD_NOW);
    if (!libgl) libgl = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!libgl) libgl = dlopen("libOpenGL.so.0", RTLD_NOW);
    if (libgl) {
        sym = dlsym(libgl, "glViewport");
        if (!sym && real_dlsym) sym = real_dlsym(libgl, "glViewport");
    }
    if (!sym) sym = dlsym(RTLD_DEFAULT, "glViewport");

    if (!sym) {
        EDBG("glViewport symbol not found");
        return -1;
    }

    int n = syringe_hook_install("glViewport",
                                  (void*)hook_glViewport,
                                  (void**)&orig_glViewport);
    if (n > 0) {
        g_glViewport_hook_installed = 1;
        EDBG("glViewport hook installed via GOT walk");
        return 0;
    }

    n = syringe_hook_install_addr("glViewport", sym,
                                   (void*)hook_glViewport,
                                   (void**)&orig_glViewport);
    if (n > 0) {
        g_glViewport_hook_installed = 1;
        EDBG("glViewport hook installed via install_addr");
        return 0;
    }

    EDBG("glViewport hook install failed");
    return -1;
}

/* ── wl_egl_window_resize hook ─────────────────────────────────────── */

typedef void (*PFN_wl_egl_window_resize_fn)(void*, int, int, int, int);
static PFN_wl_egl_window_resize_fn orig_wl_egl_window_resize = NULL;

static void hook_wl_egl_window_resize(void *win, int w, int h, int dx, int dy) {
    if (orig_wl_egl_window_resize)
        orig_wl_egl_window_resize(win, w, h, dx, dy);
    idk_compositor_notify_resize(w, h);
}

static int install_wl_egl_hook(void) {
    if (g_wl_egl_hook_installed) return 0;

    void *sym = NULL;
    int n;

    /* Method 1: dlsym(RTLD_DEFAULT) — works when symbol is in global table
     * (e.g. SDL3 statically linked against libwayland-egl) */
    sym = real_dlsym
        ? real_dlsym(RTLD_DEFAULT, "wl_egl_window_resize")
        : dlsym(RTLD_DEFAULT, "wl_egl_window_resize");
    if (sym) {
        n = syringe_hook_install_addr("wl_egl_window_resize", sym,
                                       (void*)hook_wl_egl_window_resize,
                                       (void**)&orig_wl_egl_window_resize);
        if (n > 0) {
            g_wl_egl_hook_installed = 1;
            EDBG("wl_egl_window_resize installed via RTLD_DEFAULT\n");
            return 0;
        }
    }

    /* Method 2: dlopen libwayland-egl + dlsym (traditional) */
    void *lib = real_dlopen
        ? real_dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_NOLOAD)
        : dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
        lib = real_dlopen
            ? real_dlopen("libwayland-egl.so", RTLD_NOW | RTLD_NOLOAD)
            : dlopen("libwayland-egl.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) return -1;

    sym = real_dlsym
        ? real_dlsym(lib, "wl_egl_window_resize")
        : dlsym(lib, "wl_egl_window_resize");
    if (!sym) return -1;

    n = syringe_hook_install("wl_egl_window_resize",
                              (void*)hook_wl_egl_window_resize,
                              (void**)&orig_wl_egl_window_resize);
    if (n > 0) {
        g_wl_egl_hook_installed = 1;
        EDBG("wl_egl_window_resize installed via GOT walk\n");
        return 0;
    }

    n = syringe_hook_install_addr("wl_egl_window_resize", sym,
                                   (void*)hook_wl_egl_window_resize,
                                   (void**)&orig_wl_egl_window_resize);
    if (n > 0) {
        g_wl_egl_hook_installed = 1;
        EDBG("wl_egl_window_resize installed via install_addr\n");
        return 0;
    }

    return -1;
}

int idk_egl_init(void) {
    if (g_hook_installed) return 0;
    return install_egl_hook();
}

void idk_egl_shutdown(void) {
    if (!g_hook_installed) return;

    /* During process exit, the game's EGL context is already torn down
     * BEFORE our destructor runs. Calling syringe_hook_remove restores
     * the original code but can race with the game's exit path. And
     * idk_compositor_shutdown's GL cleanup (already removed) would crash
     * with a dead context.
     *
     * Safest: just mark as shut down, don't touch hooks or GL. The OS
     * reclaims everything on process exit. */
    g_hook_installed = 0;
    g_gl_resources_ready = 0;
    g_retry_done = 0;
}
