/*
 * egl_hook.c — EGL hook for ptrace inject path (osu-lazer / Wayland / OpenGL)
 *
 * This file is compiled into libidk-overlay.so. When the .so is injected
 * into a target process (via syringe_inject), idk_egl_init() is called
 * from the constructor. It:
 *   1. dlopen("libEGL.so.1") to get a handle
 *   2. dlsym(handle, "eglSwapBuffers") to get the real function address
 *   3. syringe_hook_install_addr("eglSwapBuffers", addr, hook, &orig)
 *      — bypasses the GOT walk (which fails for osu-lazer because SDL
 *        dlopen's libEGL.so.1 itself, so eglSwapBuffers is not in the
 *        main binary's PLT)
 *      — installs an inline trampoline directly on libEGL.so code
 *
 * The hook function (hook_eglSwapBuffers) runs on every frame:
 *   1. Lazy-init compositor GL resources on first swap (GL context ready)
 *   2. Receive overlay frame from webview (DMA-BUF via Unix socket)
 *   3. Render fullscreen quad with overlay texture
 *   4. Call orig_eglSwapBuffers (game frame + overlay swapped together)
 *
 * The compositor (compositor.c) is the SAME one used by the LD_PRELOAD
 * path — it handles the DMA-BUF → EGLImage → GL texture conversion.
 *
 * Why not syringe_hook_install("eglSwapBuffers", ...)?
 *   That does GOT/PLT patching first, then falls back to inline trampoline.
 *   For osu-lazer:
 *     - GOT walk finds nothing (eglSwapBuffers not in osu!'s PLT because
 *       SDL resolves it via dlsym and stores in struct)
 *     - dlsym(RTLD_DEFAULT, "eglSwapBuffers") may return NULL or wrong addr
 *       (because libEGL.so.1 was dlopen'd with RTLD_LOCAL by SDL)
 *   So syringe_hook_install() fails. We need install_addr() with explicit
 *   address from dlopen("libEGL.so.1", RTLD_NOLOAD) + dlsym.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>

#include "hook/syringe_hook.h"
#include "idk_egl.h"
#include "public/idk_gl_loader.h"   /* GL types + function pointer redirects */
#include "../core/compositor.h"
#include "public/idk_log.h"

/* Debug log file — bypass stderr (which AppImage may redirect). */
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

/* ── EGL types (opaque — we don't need full EGL headers) ────────────────── */

typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef unsigned int EGLBoolean;

/* ── Original function pointer (filled by syringe_hook_install_addr) ────── */

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;

/* ── State ──────────────────────────────────────────────────────────────── */

static int g_initialized = 0;
static int g_gl_resources_ready = 0;

/* ── Hook implementation ──────────────────────────────────────────────────
 *
 * Called every frame by the target process. We:
 *   1. Lazy-init GL resources on first swap (GL context is now ready)
 *   2. Poll compositor for new overlay frame (non-blocking)
 *   3. Render fullscreen quad with overlay texture
 *   4. Call original eglSwapBuffers
 */

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    /* Lazy-init GL compositor resources on first swap.
     * The target's GL context is now current — safe to compile shaders
     * and create VBOs. */
    if (!g_gl_resources_ready) {
        EDBG("First swap — initializing GL resources");
        if (idk_compositor_init_gl() == 0) {
            g_gl_resources_ready = 1;
        } else {
            EDBG("GL resources init failed — overlay disabled");
        }
    }

    /* Receive overlay frame from webview (non-blocking).
     * If a new frame is available, compositor uploads it to a GL texture. */
    idk_compositor_render();

    /* Render overlay quad on top of game frame, BEFORE swap.
     * Uses GL viewport to determine quad size.
     * idk_gl_loader_init() was called by idk_compositor_init_gl() above,
     * so idk_fn_glGetIntegerv is resolved. */
    if (idk_compositor_has_overlay() && idk_fn_glGetIntegerv) {
        GLint vp[4] = {0, 0, 0, 0};
        idk_fn_glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] > 0 && vp[3] > 0) {
            idk_compositor_render_overlay(0, 0, (uint32_t)vp[2], (uint32_t)vp[3]);
        }
    }

    /* Call original eglSwapBuffers — swaps game frame + overlay together */
    if (orig_eglSwapBuffers) {
        return orig_eglSwapBuffers(dpy, surface);
    }
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int idk_egl_init(void) {
    if (g_initialized) return 0;
    g_initialized = 1;

    EDBG("Initializing EGL hook (ptrace inject path)");

    /* Step 1: Get handle to libEGL.so.1.
     * RTLD_NOLOAD — if SDL already loaded it, reuse the same handle.
     * This is critical: we want the SAME libEGL.so.1 instance that the
     * target is using, not a fresh dlopen. */
    void *egl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!egl_handle) {
        /* Not yet loaded — try loading it ourselves.
         * Use RTLD_GLOBAL so the target can also use it later. */
        egl_handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!egl_handle) {
            egl_handle = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
        }
    }
    if (!egl_handle) {
        EDBG("dlopen libEGL.so.1 failed: %s", dlerror());
        return -1;
    }
    EDBG("libEGL.so.1 handle = %p", egl_handle);

    /* Step 2: Resolve eglSwapBuffers address.
     * dlsym on our handle gives us the REAL function address inside
     * libEGL.so code — this is what we'll patch. */
    void *egl_swap_addr = dlsym(egl_handle, "eglSwapBuffers");
    if (!egl_swap_addr) {
        EDBG("dlsym eglSwapBuffers failed: %s", dlerror());
        return -1;
    }
    EDBG("eglSwapBuffers @ %p", egl_swap_addr);

    /* Step 3: Start compositor (Unix socket server for webview frames).
     * Must be done BEFORE installing the hook, so when the first swap
     * fires the compositor is ready to receive frames. */
    if (idk_compositor_init() != 0) {
        EDBG("Compositor init failed — overlay will not work");
        /* Continue anyway — hook will still install, just no overlay frames */
    }

    /* Step 4: Install inline trampoline via syringe_hook_install_addr.
     * This patches the first instructions of eglSwapBuffers in libEGL.so
     * code with a JMP to our hook. The trampoline bounce stub lets us
     * call the original function via orig_eglSwapBuffers.
     *
     * syringe_hook_install_addr is the right API here because:
     *   - syringe_hook_install("eglSwapBuffers", ...) does GOT walk first
     *     which fails for osu-lazer (SDL dlopen'd libEGL privately)
     *   - install_addr bypasses GOT, patches code directly
     *   - syringe's inline trampoline handles: length-disasm to avoid
     *     splitting instructions, RIP-relative fixup, atomic patch,
     *     /proc/self/mem fallback for r-x page */
    int n = syringe_hook_install_addr("eglSwapBuffers",
                                       egl_swap_addr,
                                       (void*)hook_eglSwapBuffers,
                                       (void**)&orig_eglSwapBuffers);
    if (n <= 0) {
        EDBG("syringe_hook_install_addr failed");
        return -1;
    }

    IDK_LOG("egl", "Hook installed — orig_eglSwapBuffers = %p\n",
            (void*)orig_eglSwapBuffers);
    return 0;
}

void idk_egl_shutdown(void) {
    if (!g_initialized) return;
    EDBG("Shutting down");
    syringe_hook_remove("eglSwapBuffers");
    idk_compositor_shutdown();
    g_initialized = 0;
    g_gl_resources_ready = 0;
}
