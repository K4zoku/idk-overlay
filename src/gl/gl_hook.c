/*
 * gl_hook.c — OpenGL hooks + SHM fallback + EGL compositor
 *
 * Hooks:
 *   - glXSwapBuffers     (GLX / X11)
 *   - eglSwapBuffers     (EGL / Wayland)
 *   - SDL_GL_SwapWindow  (SDL2/SDL3)
 *
 * For GLX/EGL: reads pixels via glReadPixels into /dev/shm,
 * then sends the SHM fd over IPC to idk-render.
 *
 * EGL compositor (Wayland): receives overlay frames from webview
 * via Unix socket, converts to GL textures via EGL DMA-BUF import,
 * and renders fullscreen quads on top of the game's framebuffer
 * BEFORE calling the original swap.
 *
 * NOTE: glReadPixels is synchronous and slow. For Vulkan apps, use
 * the Vulkan hook instead (dmabuf is zero-copy).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "hook/syringe_hook.h"
#include "gl/gl.h"
#include "public/idk_ipc.h"
#include "core/compositor.h"
#include "core/log.h"

/* ── GL function pointer types ─────────────────────────────────────── */

typedef void (*PFN_glReadPixelsFn)(int, int, int, int, int, int, void *);
typedef void (*PFN_glGetIntegervFn)(int, int *);

/* ── Internal state ────────────────────────────────────────────────── */

static int g_ipc_fd = -1;
static char g_shm_base[1024];

/* Original function pointers */
typedef void* Display;
typedef void* GLXDrawable;
typedef void (*GlXSwapBuffersFn)(Display *, GLXDrawable);
static GlXSwapBuffersFn orig_glXSwapBuffers = NULL;

typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef unsigned int EGLBoolean;
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;

static void (*orig_SDL_GL_SwapWindow)(void *) = NULL;

/* GL function pointers (resolved once) */
static PFN_glReadPixelsFn fn_glReadPixels = NULL;
static PFN_glGetIntegervFn fn_glGetIntegerv = NULL;

/* GL compositor resources — initialized lazily on first swap */
static int g_gl_resources_ready = 0;

/* ── SHM helper ────────────────────────────────────────────────────── */

/**
 * Create a named SHM file, read GL pixels into it, return fd.
 * Returns fd on success (caller must close it), -1 on failure.
 */
static int read_pixels_to_shm(uint32_t width, uint32_t height) {
    char shm_name[256];
    snprintf(shm_name, sizeof(shm_name), "/idk-gl-%d", getpid());

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        IDK_ERR("gl", "shm_open failed: %s\n", strerror(errno));
        return -1;
    }

    size_t buf_size = width * height * 4;
    if (ftruncate(shm_fd, buf_size) < 0) {
        IDK_ERR("gl", "ftruncate failed: %s\n", strerror(errno));
        close(shm_fd);
        return -1;
    }

    uint8_t *pixel_buf = malloc(buf_size);
    if (!pixel_buf) {
        IDK_ERR("gl", "malloc failed\n");
        close(shm_fd);
        return -1;
    }

    uint8_t *gl_buf = malloc(buf_size);
    if (!gl_buf) {
        free(pixel_buf);
        IDK_ERR("gl", "malloc (gl_buf) failed\n");
        close(shm_fd);
        return -1;
    }

    if (!fn_glReadPixels) {
        IDK_ERR("gl", "glReadPixels not available\n");
        free(gl_buf);
        free(pixel_buf);
        close(shm_fd);
        return -1;
    }
    fn_glReadPixels(0, 0, width, height, 0x80E3, 0x1401, gl_buf);

    /* Flip vertically + convert BGRA -> ABGR */
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst_row = &pixel_buf[y * width * 4];
        uint8_t *src_row = &gl_buf[(height - 1 - y) * width * 4];
        for (uint32_t x = 0; x < width; x++) {
            uint8_t *src = &src_row[x * 4];
            dst_row[x * 4 + 0] = src[3]; /* B -> A */
            dst_row[x * 4 + 1] = src[2]; /* G -> B */
            dst_row[x * 4 + 2] = src[1]; /* R -> G */
            dst_row[x * 4 + 3] = src[0]; /* A -> R */
        }
    }

    void *shm_map = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_map == MAP_FAILED) {
        IDK_ERR("gl", "mmap SHM failed: %s\n", strerror(errno));
        free(gl_buf);
        free(pixel_buf);
        close(shm_fd);
        return -1;
    }
    memcpy(shm_map, pixel_buf, buf_size);
    munmap(shm_map, buf_size);

    shm_unlink(shm_name);
    free(gl_buf);
    free(pixel_buf);

    return shm_fd;
}

/* ── Helper: get framebuffer size ──────────────────────────────────── */

static int get_framebuffer_size(uint32_t *w, uint32_t *h) {
    int viewport[4] = {0, 0, 640, 480};
    if (fn_glGetIntegerv) {
        fn_glGetIntegerv(0x0BA2, viewport);
    }
    *w = (uint32_t)viewport[2];
    *h = (uint32_t)viewport[3];
    return 0;
}

/* ── Hook implementations ──────────────────────────────────────────── */

static void hook_glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    if (!orig_glXSwapBuffers) {
        orig_glXSwapBuffers(dpy, drawable);
        return;
    }

    /* Send game framebuffer to idk-render */
    if (g_ipc_fd >= 0) {
        uint32_t w = 0, h = 0;
        get_framebuffer_size(&w, &h);
        if (w > 0 && h > 0) {
            int shm_fd = read_pixels_to_shm(w, h);
            if (shm_fd >= 0) {
                struct {
                    uint32_t width; uint32_t height;
                    uint32_t stride; uint32_t format;
                    uint32_t num_planes; uint32_t pid;
                } info = {
                    .width = w, .height = h, .stride = w,
                    .format = 0x34325258, .num_planes = 1, .pid = getpid(),
                };
                idk_ipc_send_frame(g_ipc_fd, &info, sizeof(info), shm_fd);
                close(shm_fd);
            }
        }
    }

    orig_glXSwapBuffers(dpy, drawable);
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!orig_eglSwapBuffers) {
        return orig_eglSwapBuffers(dpy, surface);
    }

    /* Lazy-init GL compositor resources on first swap (GL context ready) */
    if (!g_gl_resources_ready) {
        idk_gl_init_gl_resources();
        g_gl_resources_ready = 1;
    }

    /* Send game framebuffer to idk-render */
    if (g_ipc_fd >= 0) {
        uint32_t w = 0, h = 0;
        get_framebuffer_size(&w, &h);
        if (w > 0 && h > 0) {
            int shm_fd = read_pixels_to_shm(w, h);
            if (shm_fd >= 0) {
                struct {
                    uint32_t width; uint32_t height;
                    uint32_t stride; uint32_t format;
                    uint32_t num_planes; uint32_t pid;
                } info = {
                    .width = w, .height = h, .stride = w,
                    .format = 0x34325258, .num_planes = 1, .pid = getpid(),
                };
                idk_ipc_send_frame(g_ipc_fd, &info, sizeof(info), shm_fd);
                close(shm_fd);
            }
        }
    }

    /* BEFORE swap: receive overlay frame from webview and render on top */
    int ret = idk_compositor_render();
    if (ret == 0) {
        uint32_t w = 0, h = 0;
        get_framebuffer_size(&w, &h);
        if (w > 0 && h > 0) {
            idk_compositor_render_overlay(0, 0, w, h);
        }
    }

    return orig_eglSwapBuffers(dpy, surface);
}

static void hook_SDL_GL_SwapWindow(void *window) {
    if (!orig_SDL_GL_SwapWindow) {
        orig_SDL_GL_SwapWindow(window);
        return;
    }

    uint32_t w = 0, h = 0;
    get_framebuffer_size(&w, &h);
    if (w > 0 && h > 0 && g_ipc_fd >= 0) {
        int shm_fd = read_pixels_to_shm(w, h);
        if (shm_fd >= 0) {
            struct {
                uint32_t width; uint32_t height;
                uint32_t stride; uint32_t format;
                uint32_t num_planes; uint32_t pid;
            } info = {
                .width = w, .height = h, .stride = w,
                .format = 0x34325258, .num_planes = 1, .pid = getpid(),
            };
            idk_ipc_send_frame(g_ipc_fd, &info, sizeof(info), shm_fd);
            close(shm_fd);
        }
    }

    orig_SDL_GL_SwapWindow(window);
}

/* ── Public API ────────────────────────────────────────────────────── */

int idk_gl_init(int ipc_fd, const char *shm_base) {
    g_ipc_fd = ipc_fd;

    if (shm_base) {
        snprintf(g_shm_base, sizeof(g_shm_base), "%s", shm_base);
    } else {
        snprintf(g_shm_base, sizeof(g_shm_base), "/dev/shm/idk-overlay-%d", getpid());
    }

    /* Resolve GL function pointers */
    void *libgl = dlopen("libGL.so", RTLD_NOW);
    if (!libgl) libgl = dlopen("libGL.so.1", RTLD_NOW);
    if (libgl) {
        fn_glReadPixels = (PFN_glReadPixelsFn)dlsym(libgl, "glReadPixels");
        fn_glGetIntegerv = (PFN_glGetIntegervFn)dlsym(libgl, "glGetIntegerv");
    }

    /* Start listening for webview clients (for overlay frames) */
    int comp_ret = idk_compositor_init();
    if (comp_ret == 0) {
        IDK_LOG("gl", "Compositor listening for overlay frames\n");
    } else {
        IDK_ERR("gl", "WARNING: Could not start compositor (overlay disabled)\n");
    }

    /* Hook GLX */
    void *glx_ptr = dlsym(RTLD_DEFAULT, "glXSwapBuffers");
    if (glx_ptr) {
        syringe_hook_install("glXSwapBuffers", (void *)hook_glXSwapBuffers,
                              (void **)&orig_glXSwapBuffers);
    }

    /* Hook EGL */
    void *egl_ptr = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    if (egl_ptr) {
        syringe_hook_install("eglSwapBuffers", (void *)hook_eglSwapBuffers,
                              (void **)&orig_eglSwapBuffers);
    }

    /* Hook SDL */
    void *sdl_ptr = dlsym(RTLD_DEFAULT, "SDL_GL_SwapWindow");
    if (sdl_ptr) {
        syringe_hook_install("SDL_GL_SwapWindow", (void *)hook_SDL_GL_SwapWindow,
                              (void **)&orig_SDL_GL_SwapWindow);
    }

    IDK_LOG("gl", "GL hooks installed (glReadPixels=%p, glGetIntegerv=%p)\n",
            (void *)fn_glReadPixels, (void *)fn_glGetIntegerv);
    return 0;
}

void idk_gl_shutdown(void) {
    idk_compositor_shutdown();
    syringe_hook_remove("glXSwapBuffers");
    syringe_hook_remove("eglSwapBuffers");
    syringe_hook_remove("SDL_GL_SwapWindow");
}

/* ── GL resource init (called from main thread after GL context is ready) ─ */

/**
 * Initialize GL shaders/VBO. Must be called from a GL context,
 * after the target process has created its GL context.
 * Called lazily from swap hook when GL context is ready.
 */
int idk_gl_init_gl_resources(void) {
    /* Initialize GL compositor shaders (compositor already initialized in idk_gl_init) */
    return idk_compositor_init_gl();
}
