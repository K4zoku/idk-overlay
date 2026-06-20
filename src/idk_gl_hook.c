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
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

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

    fprintf(stderr, "[idk-gl] Real EGL functions loaded\n");
}

/* ── Overlay initialization ──────────────────────────────────────────── */

/**
 * Initialize the compositor (must be called from main thread).
 * Socket path is read from IDK_SOCKET env var inside idk_compositor_init().
 */
static void init_compositor(void) {
    if (g_gl_initialized) return;

    const char *env_gl = getenv("IDK_GL");
    g_enable_gl = env_gl ? atoi(env_gl) : 1;

    if (!g_enable_gl) {
        fprintf(stderr, "[idk-gl] GL disabled (IDK_GL=0)\n");
        g_gl_initialized = true;
        return;
    }

    fprintf(stderr, "[idk-gl] Initializing compositor\n");

    if (idk_compositor_init() < 0) {
        fprintf(stderr, "[idk-gl] Compositor init failed\n");
        return;
    }

    g_gl_initialized = true;
}

/* ── GL rendering hook ───────────────────────────────────────────────── */

/**
 * Render overlay on top of current frame.
 * Called from eglSwapBuffers BEFORE the real swap.
 */
static void render_overlay(void) {
    if (!g_gl_initialized) return;

    /* Try to accept client and receive frame */
    if (idk_compositor_render() < 0) {
        return; /* no frame ready */
    }

    /* Get framebuffer dimensions */
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    int fb_w = vp[2];
    int fb_h = vp[3];

    /* Render overlay if available */
    if (idk_compositor_has_overlay()) {
        /* Get overlay position from compositor (use full screen by default) */
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
        /* No hook active, call through */
        if (real_eglSwapBuffers) {
            return real_eglSwapBuffers(dpy, surface);
        }
        return 0;
    }

    /* Render overlay BEFORE swap (compositor draws on top of game) */
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

    /* Fallback: call eglGetDisplay directly */
    return eglGetDisplay((EGLNativeDisplayType)native_display);
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
    return eglGetPlatformDisplay((EGLenum)platform, native_display, attrib_list);
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

    return eglTerminate(display);
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

    return eglDestroyContext((EGLDisplay)dpy, (EGLContext)ctx);
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
