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
#include "hook/graphic_hooks.h"
#include "public/idk_ipc.h"
#include "core/log.h"
#include "shim/elfhacks.h"

typedef void (*PFN_glReadPixelsFn)(int, int, int, int, int, int, void *);
typedef void (*PFN_glGetIntegervFn)(int, int *);

static int g_ipc_fd = -1;

typedef void* Display;
typedef void* GLXDrawable;
typedef void (*GlXSwapBuffersFn)(Display *, GLXDrawable);
static GlXSwapBuffersFn orig_glXSwapBuffers = NULL;
static int g_hook_installed = 0;
static int g_retry_done = 0;

static PFN_glReadPixelsFn fn_glReadPixels = NULL;
static PFN_glGetIntegervFn fn_glGetIntegerv = NULL;

static void *(*real_dlsym)(void *, const char *) = NULL;
static void *(*real_dlopen)(const char *, int) = NULL;
static int (*real_dlclose)(void *) = NULL;

static int read_pixels_to_shm(uint32_t width, uint32_t height);

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
        IDK_LOG("glx", "real functions loaded: dlsym=%p dlopen=%p\n",
                (void *)real_dlsym, (void *)real_dlopen);
    }
}

static void send_frame(void);

static int read_pixels_to_shm(uint32_t width, uint32_t height) {
    char shm_name[256];
    snprintf(shm_name, sizeof(shm_name), "/idk-glx-%d", getpid());

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        IDK_ERR("glx", "shm_open failed: %s\n", strerror(errno));
        return -1;
    }

    size_t buf_size = width * height * 4;
    if (ftruncate(shm_fd, buf_size) < 0) {
        IDK_ERR("glx", "ftruncate failed: %s\n", strerror(errno));
        close(shm_fd);
        return -1;
    }

    uint8_t *pixel_buf = malloc(buf_size);
    if (!pixel_buf) {
        IDK_ERR("glx", "malloc failed\n");
        close(shm_fd);
        return -1;
    }

    uint8_t *gl_buf = malloc(buf_size);
    if (!gl_buf) {
        free(pixel_buf);
        IDK_ERR("glx", "malloc (gl_buf) failed\n");
        close(shm_fd);
        return -1;
    }

    if (!fn_glReadPixels) {
        IDK_ERR("glx", "glReadPixels not available\n");
        free(gl_buf);
        free(pixel_buf);
        close(shm_fd);
        return -1;
    }
    fn_glReadPixels(0, 0, width, height, 0x80E3, 0x1401, gl_buf);

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst_row = &pixel_buf[y * width * 4];
        uint8_t *src_row = &gl_buf[(height - 1 - y) * width * 4];
        for (uint32_t x = 0; x < width; x++) {
            uint8_t *src = &src_row[x * 4];
            dst_row[x * 4 + 0] = src[3];
            dst_row[x * 4 + 1] = src[2];
            dst_row[x * 4 + 2] = src[1];
            dst_row[x * 4 + 3] = src[0];
        }
    }

    void *shm_map = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_map == MAP_FAILED) {
        IDK_ERR("glx", "mmap SHM failed: %s\n", strerror(errno));
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

static int get_framebuffer_size(uint32_t *w, uint32_t *h) {
    int viewport[4] = {0, 0, 640, 480};
    if (fn_glGetIntegerv) {
        fn_glGetIntegerv(0x0BA2, viewport);
    }
    *w = (uint32_t)viewport[2];
    *h = (uint32_t)viewport[3];
    return 0;
}

static void send_frame(void) {
    if (g_ipc_fd < 0) return;
    uint32_t w = 0, h = 0;
    get_framebuffer_size(&w, &h);
    if (w == 0 || h == 0) return;

    int shm_fd = read_pixels_to_shm(w, h);
    if (shm_fd < 0) return;

    idk_frame_header_t hdr = { 0 };
    hdr.width  = w;
    hdr.height = h;
    hdr.stride = 0;  /* SHM: no stride */
    hdr.flags  = IDK_FRAME_FLAG_VISIBLE;
    idk_ipc_send_frame(g_ipc_fd, &hdr, shm_fd);
    close(shm_fd);
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
    g_hook_installed = 1;

    void *libgl = real_dlopen ? real_dlopen("libGL.so", RTLD_NOW) : dlopen("libGL.so", RTLD_NOW);
    if (!libgl) libgl = real_dlopen ? real_dlopen("libGL.so.1", RTLD_NOW) : dlopen("libGL.so.1", RTLD_NOW);
    if (libgl) {
        fn_glReadPixels = (PFN_glReadPixelsFn)(real_dlsym ? real_dlsym(libgl, "glReadPixels") : dlsym(libgl, "glReadPixels"));
        fn_glGetIntegerv = (PFN_glGetIntegervFn)(real_dlsym ? real_dlsym(libgl, "glGetIntegerv") : dlsym(libgl, "glGetIntegerv"));
    }

    static const char *glx_libs[] = {"libGLX.so.0", "libGL.so.1", NULL};
    void *glx_sym = NULL;
    for (int i = 0; glx_libs[i]; i++) {
        void *h = real_dlopen ? real_dlopen(glx_libs[i], RTLD_NOW | RTLD_NOLOAD) : dlopen(glx_libs[i], RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = real_dlopen ? real_dlopen(glx_libs[i], RTLD_NOW | RTLD_GLOBAL) : dlopen(glx_libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            continue;
        glx_sym = real_dlsym ? real_dlsym(h, "glXSwapBuffers") : dlsym(h, "glXSwapBuffers");
        if (glx_sym) break;
    }

    int n = 0;
    if (glx_sym) {
        n = syringe_hook_install_addr("glXSwapBuffers", glx_sym,
                                      (void *)hook_glXSwapBuffers,
                                      (void **)&orig_glXSwapBuffers);
        if (n > 0) {
            IDK_LOG("glx", "installed via install_addr");
            return 0;
        }
    }

    n = syringe_hook_install("glXSwapBuffers",
                             (void *)hook_glXSwapBuffers,
                             (void **)&orig_glXSwapBuffers);
    if (n > 0) {
        IDK_LOG("glx", "installed via GOT walk");
        return 0;
    }

    if (glx_sym) {
        n = syringe_hook_install_addr("glXSwapBuffers", glx_sym,
                                      (void *)hook_glXSwapBuffers,
                                      (void **)&orig_glXSwapBuffers);
        if (n > 0) {
            IDK_LOG("glx", "installed via install_addr (retry)");
            return 0;
        }
    }

    IDK_ERR("glx", "all install methods failed, deferring to first call");
    return -1;
}

static void hook_glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    if (!orig_glXSwapBuffers && !g_retry_done) {
        g_retry_done = 1;
        install_glx_hook();
    }

    send_frame();
    if (orig_glXSwapBuffers) {
        orig_glXSwapBuffers(dpy, drawable);
    } else {
        call_real_glXSwapBuffers(dpy, drawable);
    }
}

int idk_glx_init(int ipc_fd) {
    g_ipc_fd = ipc_fd;
    if (g_hook_installed) return 0;
    return install_glx_hook();
}

void idk_glx_shutdown(void) {
    syringe_hook_remove("glXSwapBuffers");
    g_hook_installed = 0;
    g_retry_done = 0;
}
