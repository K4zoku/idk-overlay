/*
 * idk_gl_hook.c — GL/EGL hooks for idk-overlay
 *
 * This library is loaded by idk_shim.so at runtime. It provides:
 *   - idk_eglSwapBuffers()   — render overlay after real swap
 *   - idk_eglGetProcAddress() — resolve GL functions + return hooks
 *   - idk_eglGetDisplay()     — wrapper around real eglGetDisplay
 *   - idk_eglGetPlatformDisplay() — wrapper around real eglGetPlatformDisplay
 *   - idk_eglTerminate()      — wrapper around real eglTerminate
 *   - idk_eglDestroyContext() — wrapper around real eglDestroyContext
 *
 * The compositor (from compositor.c) handles:
 *   - Receiving DMA-BUF frames from webview
 *   - Converting to GL textures
 *   - Rendering overlay quads in the swap hook
 *
 * Architecture (inspired by MangoHud):
 *   Shim intercepts dlsym → returns hook function pointers
 *   Hook functions call real GL + our overlay rendering
 *   Overlay rendered AFTER real swap (on top of game frame)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>

#include "idk_gl_loader.h"   /* GL types + function pointer redirects */
#include "compositor.h"
#include "idk_ipc.h"

/* ── Forward declarations ────────────────────────────────────────────── */

void *idk_eglGetProcAddress(const char *procName);
void *idk_eglGetDisplay(void *native_display);
void *idk_eglGetPlatformDisplay(unsigned int platform,
                                 void *native_display,
                                 const intptr_t *attrib_list);
int idk_eglTerminate(void *display);
unsigned int idk_eglDestroyContext(void *dpy, void *ctx);
unsigned int idk_eglSwapBuffers(void *dpy, void *surface);

/* ── Real function pointers ──────────────────────────────────────────── */

static void *(*real_eglGetProcAddress)(const char *) = NULL;
static unsigned int (*real_eglSwapBuffers)(void *dpy, void *surface) = NULL;
static void *(*real_eglGetDisplay)(void *) = NULL;
static void *(*real_eglGetPlatformDisplay)(unsigned int, void *, const intptr_t *) = NULL;
static int (*real_eglTerminate)(void *) = NULL;
static unsigned int (*real_eglDestroyContext)(void *, void *) = NULL;

/* ── EGL function pointers (for eglQuerySurface) ────────────────────── */
typedef int (*PFN_eglQuerySurface)(void *dpy, void *surface, int attribute, int *value);
static PFN_eglQuerySurface fn_eglQuerySurface = NULL;

#define EGL_WIDTH  0x3057
#define EGL_HEIGHT 0x3056

/* ── GL state ────────────────────────────────────────────────────────── */

static bool g_gl_initialized = false;
static int g_enable_gl = 1;

/* ── EGL function resolution ─────────────────────────────────────────── */

/**
 * Resolve EGL/GL function addresses (like MangoHud's get_egl_proc_address).
 * Uses multiple fallback strategies.
 */
static void *resolve_egl_function(const char *name) {
    /* Strategy 1: Use real eglGetProcAddress if available */
    if (real_eglGetProcAddress) {
        void *fn = real_eglGetProcAddress(name);
        if (fn) return fn;
    }

    /* Strategy 2: Load from libEGL.so.1 directly */
    void *lib_egl = dlopen("libEGL.so.1", RTLD_LAZY);
    if (lib_egl) {
        void *fn = dlsym(lib_egl, name);
        dlclose(lib_egl);
        if (fn) return fn;
    }

    /* Strategy 3: Try libEGL.so (without version) */
    lib_egl = dlopen("libEGL.so", RTLD_LAZY);
    if (lib_egl) {
        void *fn = dlsym(lib_egl, name);
        dlclose(lib_egl);
        if (fn) return fn;
    }

    return NULL;
}

/**
 * Load all real EGL functions on first use.
 */
static void load_real_functions(void) {
    if (real_eglGetProcAddress) return;

    real_eglGetProcAddress = (typeof(real_eglGetProcAddress))resolve_egl_function("eglGetProcAddress");
    real_eglSwapBuffers = (typeof(real_eglSwapBuffers))resolve_egl_function("eglSwapBuffers");
    real_eglGetDisplay = (typeof(real_eglGetDisplay))resolve_egl_function("eglGetDisplay");
    real_eglGetPlatformDisplay = (typeof(real_eglGetPlatformDisplay))resolve_egl_function("eglGetPlatformDisplay");
    real_eglTerminate = (typeof(real_eglTerminate))resolve_egl_function("eglTerminate");
    real_eglDestroyContext = (typeof(real_eglDestroyContext))resolve_egl_function("eglDestroyContext");
    fn_eglQuerySurface = (PFN_eglQuerySurface)resolve_egl_function("eglQuerySurface");

    fprintf(stderr, "[idk-gl] Real EGL functions loaded\n");
}

/* ── Overlay initialization ──────────────────────────────────────────── */

/**
 * Initialize the compositor (must be called from main thread).
 * Socket path is read from IDK_SOCKET env var inside idk_compositor_init().
 * GL resources (shaders, VBO) are init'd lazily on first swap.
 */
static int g_compositor_inited = 0;
static int g_gl_resources_ready = 0;

static void init_compositor(void) {
    if (g_gl_initialized) return;

    const char *env_gl = getenv("IDK_GL");
    g_enable_gl = env_gl ? atoi(env_gl) : 1;

    if (!g_enable_gl) {
        fprintf(stderr, "[idk-gl] GL disabled (IDK_GL=0)\n");
        g_gl_initialized = true;
        return;
    }

    if (!g_compositor_inited) {
        fprintf(stderr, "[idk-gl] Initializing compositor\n");
        if (idk_compositor_init() < 0) {
            fprintf(stderr, "[idk-gl] Compositor init failed\n");
            return;
        }
        g_compositor_inited = 1;
    }

    g_gl_initialized = true;
}

/* ── GL rendering hook ───────────────────────────────────────────────── */

/**
 * Render overlay on top of current frame.
 * Called from eglSwapBuffers BEFORE the real swap.
 * Uses eglQuerySurface to get dimensions (MangoHud approach — more
 * reliable than glGetIntegerv(GL_VIEWPORT) which can return 0,0,0,0).
 */
static void *g_current_dpy = NULL;
static void *g_current_surface = NULL;

static void render_overlay(void) {
    if (!g_gl_initialized) return;

    /* Poll for new frame from webview client (non-blocking).
     * This updates the texture if a new frame arrived. */
    idk_compositor_render();

    /* Only render the overlay quad if we have a texture */
    if (!idk_compositor_has_overlay()) return;

    /* Get surface dimensions via eglQuerySurface (MangoHud approach) */
    int fb_w = 0, fb_h = 0;
    if (fn_eglQuerySurface && g_current_dpy && g_current_surface) {
        fn_eglQuerySurface(g_current_dpy, g_current_surface, EGL_HEIGHT, &fb_h);
        fn_eglQuerySurface(g_current_dpy, g_current_surface, EGL_WIDTH, &fb_w);
    }

    /* Fallback to glGetIntegerv if eglQuerySurface failed */
    if (fb_w <= 0 || fb_h <= 0) {
        GLint vp[4] = {0, 0, 0, 0};
        if (idk_fn_glGetIntegerv) {
            idk_fn_glGetIntegerv(GL_VIEWPORT, vp);
        }
        fb_w = vp[2];
        fb_h = vp[3];
    }

    if (fb_w > 0 && fb_h > 0) {
        static int s_vp_debug = 0;
        if (s_vp_debug++ % 120 == 0) {
            fprintf(stderr, "[idk-gl] render_overlay: fb=%dx%d has_overlay=%d\n",
                    fb_w, fb_h, idk_compositor_has_overlay());
        }
        idk_compositor_render_overlay(0, 0, fb_w, fb_h);
    }
}

/* ── Exported hook functions (called by shim) ────────────────────────── */

/**
 * Hooked eglGetProcAddress — intercept GL function resolution.
 * Returns our hook functions when asked for them.
 */
void *idk_eglGetProcAddress(const char *procName) {
    load_real_functions();

    /* Return our own hooks if requested */
    if (strcmp(procName, "eglSwapBuffers") == 0) {
        return (void *)idk_eglSwapBuffers;
    }
    if (strcmp(procName, "eglGetDisplay") == 0) {
        return (void *)idk_eglGetDisplay;
    }
    if (strcmp(procName, "eglGetPlatformDisplay") == 0) {
        return (void *)idk_eglGetPlatformDisplay;
    }
    if (strcmp(procName, "eglTerminate") == 0) {
        return (void *)idk_eglTerminate;
    }
    if (strcmp(procName, "eglDestroyContext") == 0) {
        return (void *)idk_eglDestroyContext;
    }

    /* For all other functions, use real eglGetProcAddress */
    if (real_eglGetProcAddress) {
        return real_eglGetProcAddress(procName);
    }

    /* Fallback: try to resolve directly */
    return resolve_egl_function(procName);
}

/**
 * Hooked eglSwapBuffers — render overlay after real swap.
 * This is the main render hook point.
 */
unsigned int idk_eglSwapBuffers(void *dpy, void *surface) {
    load_real_functions();
    init_compositor();

    if (!g_enable_gl || !real_eglSwapBuffers) {
        if (real_eglSwapBuffers) {
            return real_eglSwapBuffers(dpy, surface);
        }
        return 0;
    }

    /* Lazy-init GL resources on first swap (GL context is now current) */
    if (!g_gl_resources_ready) {
        if (idk_compositor_init_gl() == 0) {
            g_gl_resources_ready = 1;
            fprintf(stderr, "[idk-gl] GL resources ready\n");
        }
    }

    /* Save dpy/surface for eglQuerySurface in render_overlay */
    g_current_dpy = dpy;
    g_current_surface = surface;

    /* Render overlay BEFORE swap (MangoHud approach).
     * Overlay is drawn on top of game's frame, then both are swapped
     * to the screen together. We do NOT call glViewport — we use
     * the game's viewport and calculate NDC coords based on
     * eglQuerySurface dimensions. */
    render_overlay();

    /* Call real eglSwapBuffers */
    return real_eglSwapBuffers(dpy, surface);
}

/**
 * Hooked eglGetDisplay — wrapper around real eglGetDisplay.
 */
void *idk_eglGetDisplay(void *native_display) {
    load_real_functions();

    if (real_eglGetDisplay) {
        return real_eglGetDisplay(native_display);
    }

    /* Fallback: dlopen + dlsym eglGetDisplay */
    void *lib = dlopen("libEGL.so.1", RTLD_LAZY);
    if (lib) {
        void *(*fn)(void*) = (void*(*)(void*))dlsym(lib, "eglGetDisplay");
        if (fn) return fn(native_display);
    }
    return NULL;
}

/**
 * Hooked eglGetPlatformDisplay — wrapper around real eglGetPlatformDisplay.
 */
void *idk_eglGetPlatformDisplay(unsigned int platform,
                                 void *native_display,
                                 const intptr_t *attrib_list) {
    load_real_functions();

    if (real_eglGetPlatformDisplay) {
        return real_eglGetPlatformDisplay(platform, native_display, attrib_list);
    }

    /* Fallback */
    void *lib = dlopen("libEGL.so.1", RTLD_LAZY);
    if (lib) {
        void *(*fn)(unsigned int, void*, const intptr_t*) =
            (void*(*)(unsigned int, void*, const intptr_t*))dlsym(lib, "eglGetPlatformDisplay");
        if (fn) return fn(platform, native_display, attrib_list);
    }
    return NULL;
}

/**
 * Hooked eglTerminate — wrapper around real eglTerminate.
 */
int idk_eglTerminate(void *display) {
    load_real_functions();

    if (real_eglTerminate) {
        int ret = real_eglTerminate(display);
        g_gl_initialized = false; /* reset on terminate */
        return ret;
    }

    void *lib = dlopen("libEGL.so.1", RTLD_LAZY);
    if (lib) {
        int (*fn)(void*) = (int(*)(void*))dlsym(lib, "eglTerminate");
        if (fn) return fn(display);
    }
    return 0;
}

/**
 * Hooked eglDestroyContext — wrapper around real eglDestroyContext.
 */
unsigned int idk_eglDestroyContext(void *dpy, void *ctx) {
    load_real_functions();

    if (real_eglDestroyContext) {
        unsigned int ret = real_eglDestroyContext(dpy, ctx);
        g_gl_initialized = false; /* reset on context destroy */
        return ret;
    }

    void *lib = dlopen("libEGL.so.1", RTLD_LAZY);
    if (lib) {
        unsigned int (*fn)(void*, void*) =
            (unsigned int(*)(void*, void*))dlsym(lib, "eglDestroyContext");
        if (fn) return fn(dpy, ctx);
    }
    return 0;
}

/* ── Library constructor/destructor ──────────────────────────────────── */

__attribute__((constructor))
static void gl_hook_init(void) {
    fprintf(stderr, "[idk-gl] Library loaded (constructor)\n");
}

__attribute__((destructor))
static void gl_hook_shutdown(void) {
    fprintf(stderr, "[idk-gl] Library unloaded (destructor)\n");
}
