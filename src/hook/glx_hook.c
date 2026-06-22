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
#include "hook/glx.h"
#include "public/idk_ipc.h"
#include "core/log.h"

typedef void (*PFN_glReadPixelsFn)(int, int, int, int, int, int, void *);
typedef void (*PFN_glGetIntegervFn)(int, int *);

static int g_ipc_fd = -1;

typedef void* Display;
typedef void* GLXDrawable;
typedef void (*GlXSwapBuffersFn)(Display *, GLXDrawable);
static GlXSwapBuffersFn orig_glXSwapBuffers = NULL;

static PFN_glReadPixelsFn fn_glReadPixels = NULL;
static PFN_glGetIntegervFn fn_glGetIntegerv = NULL;

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

static void hook_glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    send_frame();
    orig_glXSwapBuffers(dpy, drawable);
}

int idk_glx_init(int ipc_fd) {
    g_ipc_fd = ipc_fd;

    void *libgl = dlopen("libGL.so", RTLD_NOW);
    if (!libgl) libgl = dlopen("libGL.so.1", RTLD_NOW);
    if (libgl) {
        fn_glReadPixels = (PFN_glReadPixelsFn)dlsym(libgl, "glReadPixels");
        fn_glGetIntegerv = (PFN_glGetIntegervFn)dlsym(libgl, "glGetIntegerv");
    }

    static const char *glx_libs[] = {"libGLX.so.0", "libGL.so.1", NULL};
    for (int i = 0; glx_libs[i]; i++) {
        void *h = dlopen(glx_libs[i], RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = dlopen(glx_libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            continue;
        void *p = dlsym(h, "glXSwapBuffers");
        if (p) {
            syringe_hook_install_addr("glXSwapBuffers", p,
                                      (void *)hook_glXSwapBuffers,
                                      (void **)&orig_glXSwapBuffers);
            break;
        }
    }

    return 0;
}

void idk_glx_shutdown(void) {
    syringe_hook_remove("glXSwapBuffers");
}
