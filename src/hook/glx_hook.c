/* glx_hook.c — GLX swap hook for overlay compositing.
 *
 * Mirrors the EGL hook: hooks glXSwapBuffers, inits compositor,
 * per-swap calls idk_compositor_render() (receive overlay frame from
 * webview) + idk_compositor_render_overlay() (draw overlay on top).
 *
 * The compositor code (src/core/compositor.c) uses gl/gl_loader.h
 * function pointers which work under any current GL context — GLX
 * or EGL. No GLX-specific GL code needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "hook/syringe_hook.h"
#include "hook/graphic_hooks.h"
#include "core/compositor.h"
#include "core/log.h"
#include "shim/elfhacks.h"
#include "gl/gl_loader.h"

typedef void* Display;
typedef void* GLXDrawable;
typedef void (*GlXSwapBuffersFn)(Display *, GLXDrawable);

static GlXSwapBuffersFn orig_glXSwapBuffers = NULL;
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
}

static void hook_glXSwapBuffers(Display *dpy, GLXDrawable drawable);
static int install_glx_hook(void);

static void call_real_glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    load_real_functions();
    if (real_dlsym) {
        void *lib = dlopen("libGLX.so.0", RTLD_LAZY);
        if (!lib) lib = dlopen("libGL.so.1", RTLD_LAZY);
        if (lib) {
            void *sym = real_dlsym(lib, "glXSwapBuffers");
            if (sym) {
                GlXSwapBuffersFn fn = (GlXSwapBuffersFn)sym;
                fn(dpy, drawable);
            }
            if (real_dlclose) real_dlclose(lib);
        }
    }
}

static int install_glx_hook(void) {
    load_real_functions();
    if (g_hook_installed) return 0;

    void *glx_sym = NULL;
    static const char *glx_libs[] = {"libGLX.so.0", "libGL.so.1", NULL};
    for (int i = 0; glx_libs[i]; i++) {
        void *h = real_dlopen ? real_dlopen(glx_libs[i], RTLD_NOW | RTLD_NOLOAD)
                              : dlopen(glx_libs[i], RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = real_dlopen ? real_dlopen(glx_libs[i], RTLD_NOW | RTLD_GLOBAL)
                            : dlopen(glx_libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h) continue;
        glx_sym = real_dlsym ? real_dlsym(h, "glXSwapBuffers")
                             : dlsym(h, "glXSwapBuffers");
        if (glx_sym) break;
    }

    if (!glx_sym) {
        IDK_ERR("glx", "glXSwapBuffers not found\n");
        return -1;
    }

    /* Don't init compositor here — no GL context current on background thread.
     * Compositor init happens in hook_glXSwapBuffers (like EGL does). */

    int n = syringe_hook_install_addr("glXSwapBuffers", glx_sym,
                                      (void *)hook_glXSwapBuffers,
                                      (void **)&orig_glXSwapBuffers);
    if (n > 0) {
        g_hook_installed = 1;
        IDK_LOG("glx", "installed via install_addr\n");
        return 0;
    }

    n = syringe_hook_install("glXSwapBuffers",
                             (void *)hook_glXSwapBuffers,
                             (void **)&orig_glXSwapBuffers);
    if (n > 0) {
        g_hook_installed = 1;
        IDK_LOG("glx", "installed via GOT walk\n");
        return 0;
    }

    IDK_ERR("glx", "all install methods failed\n");
    return -1;
}

static int g_compositor_inited = 0;

static void hook_glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    if (!orig_glXSwapBuffers && !g_retry_done) {
        g_retry_done = 1;
        install_glx_hook();
    }

    /* Init compositor (socket) once — safe from hook (main thread) */
    if (!g_compositor_inited) {
        if (idk_compositor_init() == 0) {
            g_compositor_inited = 1;
        } else {
            IDK_ERR("glx", "compositor init failed\n");
        }
    }

    /* Init GL resources (shaders, VBO) once — needs a current GL context.
     * glXSwapBuffers always has a current context. */
    if (g_compositor_inited && !g_gl_resources_ready) {
        if (idk_compositor_init_gl() == 0) {
            g_gl_resources_ready = 1;
        }
    }

    /* Receive overlay frame from webview (non-blocking) */
    idk_compositor_render();

    /* Render overlay on top of game's framebuffer */
    if (idk_compositor_has_overlay() && idk_fn_glGetIntegerv) {
        GLint vp[4] = {0, 0, 0, 0};
        idk_fn_glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] > 0 && vp[3] > 0) {
            idk_compositor_notify_resize((int)vp[2], (int)vp[3]);
            idk_compositor_render_overlay(0, 0, (uint32_t)vp[2], (uint32_t)vp[3]);
        }
    }

    /* Call real glXSwapBuffers to present */
    if (orig_glXSwapBuffers) {
        orig_glXSwapBuffers(dpy, drawable);
    } else {
        call_real_glXSwapBuffers(dpy, drawable);
    }
}

int idk_glx_init(void) {
    if (g_hook_installed) return 0;
    return install_glx_hook();
}

void idk_glx_shutdown(void) {
    syringe_hook_remove("glXSwapBuffers");
    g_hook_installed = 0;
    g_retry_done = 0;
    g_gl_resources_ready = 0;
}
