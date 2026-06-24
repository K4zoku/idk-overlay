/* compositor.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "gl/gl_loader.h"        /* GL types + function pointer redirects */
#include "gl/shader_loader.h"    /* shader compile + SPIR-V fallback */
#include "core/compositor.h"
#include "public/idk_ipc.h"
#include "core/log.h"

#define IDK_FRAME_TYPE_DMABUF  0
#define IDK_FRAME_TYPE_SHM     1

struct frame_hdr {
    uint32_t width;
    uint32_t height;
    uint32_t stride;       /* DMABUF: stride in bytes, SHM: unused */
    uint32_t format;       /* DMABUF: DRM fourcc, SHM: unused */
    uint32_t num_planes;   /* SHM: buffer index, DMABUF: number of planes */
    uint32_t reserved;     /* SHM: pixel byte size, DMABUF: unused */
    uint32_t vis_type;     /* visibility (low byte) + frame type (high byte) */
    uint32_t checksum;
};

typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef void* EGLImageKHR;
typedef int32_t EGLint;
typedef uint32_t EGLenum;
typedef uint32_t EGLBoolean;
typedef intptr_t EGLNativeDisplayType;

#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_NO_IMAGE_KHR    ((EGLImageKHR)0)
#define EGL_NONE            0x3038
#define EGL_WIDTH           0x3057
#define EGL_HEIGHT          0x3056
#define EGL_LINUX_DMA_BUF_EXT          0x3270
#define EGL_LINUX_DRM_FOURCC_EXT       0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT      0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT  0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT   0x3274

typedef EGLDisplay (*PFN_eglGetDisplay_fn)(EGLNativeDisplayType);
typedef EGLDisplay (*PFN_eglGetCurrentDisplay_fn)(void);
typedef EGLint (*PFN_eglGetError_fn)(void);
typedef void* (*PFN_eglGetProcAddress_fn)(const char*);
typedef EGLImageKHR (*PFN_eglCreateImageKHR_fn)(EGLDisplay dpy, EGLContext ctx,
                                                 EGLenum target,
                                                 void *buffer,
                                                 const EGLint *attrs);
typedef EGLBoolean (*PFN_eglDestroyImageKHR_fn)(EGLDisplay dpy, EGLImageKHR image);

static PFN_eglGetDisplay_fn          fn_eglGetDisplay          = NULL;
static PFN_eglGetCurrentDisplay_fn   fn_eglGetCurrentDisplay   = NULL;
static PFN_eglGetError_fn            fn_eglGetError            = NULL;
static PFN_eglGetProcAddress_fn      fn_eglGetProcAddress      = NULL;
static PFN_eglCreateImageKHR_fn      fn_eglCreateImageKHR      = NULL;
static PFN_eglDestroyImageKHR_fn     fn_eglDestroyImageKHR     = NULL;

static void resolve_egl_functions(void) {
    if (fn_eglGetDisplay) return;
    void *lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libEGL.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libEGL.so", RTLD_NOW);
    if (!lib) {
        IDK_ERR("comp", "dlopen libEGL failed: %s\n", dlerror());
        return;
    }
    fn_eglGetDisplay        = (PFN_eglGetDisplay_fn)         dlsym(lib, "eglGetDisplay");
    fn_eglGetCurrentDisplay = (PFN_eglGetCurrentDisplay_fn)  dlsym(lib, "eglGetCurrentDisplay");
    fn_eglGetError          = (PFN_eglGetError_fn)           dlsym(lib, "eglGetError");
    fn_eglGetProcAddress    = (PFN_eglGetProcAddress_fn)     dlsym(lib, "eglGetProcAddress");

    /* eglCreateImageKHR / eglDestroyImageKHR are extension functions —
     * must use eglGetProcAddress, they may not be exported from libEGL.so */
    if (fn_eglGetProcAddress) {
        fn_eglCreateImageKHR  = (PFN_eglCreateImageKHR_fn)fn_eglGetProcAddress("eglCreateImageKHR");
        fn_eglDestroyImageKHR = (PFN_eglDestroyImageKHR_fn)fn_eglGetProcAddress("eglDestroyImageKHR");
    }

    /* Fallback: try direct dlsym for drivers that do export these symbols */
    if (!fn_eglCreateImageKHR)
        fn_eglCreateImageKHR  = (PFN_eglCreateImageKHR_fn) dlsym(lib, "eglCreateImageKHR");
    if (!fn_eglDestroyImageKHR)
        fn_eglDestroyImageKHR = (PFN_eglDestroyImageKHR_fn)dlsym(lib, "eglDestroyImageKHR");

    IDK_LOG("comp", "EGL functions: eglGetDisplay=%p eglCreateImageKHR=%p\n",
            (void*)fn_eglGetDisplay, (void*)fn_eglCreateImageKHR);
}

static GLuint g_program = 0;


/* Double-buffered textures */
static GLuint g_tex[2] = {0, 0};
static int    g_tex_w[2] = {0, 0};
static int    g_tex_h[2] = {0, 0};
static int g_tex_idx = 0;   /* index of current display texture (0 or 1) */
static bool g_has_frame = false;

/* Draw error counter — reset on new frame upload, invalidate tex at 5 */
static int g_draw_err_count = 0;

static int    g_shm_fd = -1;
static void  *g_shm_map = NULL;
static size_t g_shm_map_size = 0;

static bool g_is_gles = false;
static int g_gl_version = 0;  /* major*100 + minor*10, e.g. 330 for GL 3.3 */

static int g_dmabuf_fd = -1;
static uint32_t g_frame_w = 0;
static uint32_t g_frame_h = 0;

/* Game surface size, updated by wl_egl/vk createSwapchain hooks */
static int g_game_w = 0;
static int g_game_h = 0;
static bool g_size_pending = false;
static struct timespec g_last_frame_ts = {0};

void idk_compositor_notify_resize(int w, int h) {
    if (w < 1 || h < 1) return;
    if (w != g_game_w || h != g_game_h) {
        IDK_LOG("comp", "game surface resize: %dx%d -> %dx%d\n",
                g_game_w, g_game_h, w, h);
        g_game_w = w;
        g_game_h = h;
        g_size_pending = true;
    }
}

typedef void* GLeglImage;
typedef void (*PFN_glEGLImageTargetTexture2DOES_fn)(GLenum target, GLeglImage image);
static PFN_glEGLImageTargetTexture2DOES_fn fn_glEGLImageTargetTexture2DOES = NULL;

/* GL_EXT_EGL_image_storage */
typedef void (*PFN_glEGLImageTargetTexStorageEXT_fn)(GLenum target, GLeglImage image, const GLint* attrib_list);
static PFN_glEGLImageTargetTexStorageEXT_fn fn_glEGLImageTargetTexStorageEXT = NULL;

/* Forward declaration */
static GLuint egl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h,
                                    uint32_t stride, uint32_t format);

static int g_listen_fd = -1;
static int g_client_fd = -1;
static char g_sock_path[512];

/* Init compositor — bind socket, accept client */
int idk_compositor_init(void) {
    const char *env_sock = getenv("IDK_SOCKET");
    if (env_sock && env_sock[0] != '\0') {
        snprintf(g_sock_path, sizeof(g_sock_path), "%s", env_sock);
    } else {
        snprintf(g_sock_path, sizeof(g_sock_path), "/tmp/idk-overlay-%d", getpid());
    }

    {
        struct stat st;
        if (stat(g_sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un addr = { .sun_family = AF_UNIX };
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", g_sock_path);
            if (connect(test_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                IDK_ERR("comp", "Another instance is alive — compositor disabled\n");
                close(test_fd);
                return -1;
            }
            close(test_fd);
        }
    }

    unlink(g_sock_path);

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        IDK_ERR("comp", "socket() failed: %s\n", strerror(errno));
        return -1;
    }

    int val = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    /* Make accept() non-blocking so init() returns immediately */
    int flags = fcntl(g_listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(g_listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", g_sock_path);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        unlink(g_sock_path);
        if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            if (errno == EADDRINUSE) {
                /* Another instance owns this socket — compositor disabled */
                IDK_ERR("comp", "Another instance owns the socket, compositor disabled\n");
                close(g_listen_fd);
                g_listen_fd = -1;
                return -1;
            }
            IDK_ERR("comp", "bind() failed: %s\n", strerror(errno));
            close(g_listen_fd);
            g_listen_fd = -1;
            return -1;
        }
    }

    if (listen(g_listen_fd, 4) < 0) {
        IDK_ERR("comp", "listen() failed: %s\n", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    g_client_fd = accept(g_listen_fd, NULL, NULL);
    if (g_client_fd < 0) {
        IDK_LOG("comp", "No client yet (fd=-1, will retry next frame, err=%s)\n",
                strerror(errno));
        g_client_fd = -1;
    }

    g_has_frame = false;

    IDK_LOG("comp", "Listening on %s, waiting for webview client...\n", g_sock_path);
    return 0;
}

/* Non-blocking accept */
static int accept_client(void) {
    if (g_client_fd >= 0) return 0; /* already have a client */
    if (g_listen_fd < 0) return -1;

    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int client = accept(g_listen_fd, (struct sockaddr *)&addr, &addrlen);
    if (client >= 0) {
        IDK_LOG("comp", "Client connected (fd=%d)\n", client);
        g_client_fd = client;
    }
    return 0;
}

/* SHM → GL texture upload via glTex(Sub)Image2D */
static GLuint shm_to_texture(int shm_fd, uint32_t w, uint32_t h,
                              uint32_t pixel_size, uint32_t buffer_idx) {
    if (!idk_fn_glTexImage2D) {
        IDK_ERR("comp", "glTexImage2D not resolved\n");
        return 0;
    }

    if (w == 0 || h == 0) {
        fprintf(stderr,
            "[idk-comp] shm_to_texture: rejecting zero-dim frame w=%u h=%u\n",
            w, h);
        return 0;
    }
    uint32_t expected = w * h * 4;  /* RGBA8888 = 4 bytes/pixel */
    if (pixel_size < expected) {
        fprintf(stderr,
            "[idk-comp] shm_to_texture: size mismatch w=%u h=%u pixel_size=%u expected=%u\n",
            w, h, pixel_size, expected);
        return 0;
    }
    struct stat st;
    if (fstat(shm_fd, &st) < 0) {
        IDK_ERR("comp", "SHM fstat failed: %s\n", strerror(errno));
        return 0;
    }
    static ino_t s_shm_ino = 0;
    static dev_t s_shm_dev = 0;

    if (st.st_ino != s_shm_ino || st.st_dev != s_shm_dev) {
        if (g_shm_map) {
            munmap(g_shm_map, g_shm_map_size);
            g_shm_map = NULL;
        }
        off_t total = lseek(shm_fd, 0, SEEK_END);
        if (total <= 0) total = (off_t)pixel_size;

        g_shm_map_size = (size_t)total;
        g_shm_map = mmap(NULL, g_shm_map_size, PROT_READ, MAP_SHARED, shm_fd, 0);
        if (g_shm_map == MAP_FAILED) {
            IDK_ERR("comp", "SHM mmap failed: %s\n", strerror(errno));
            g_shm_map = NULL;
            return 0;
        }
        s_shm_ino = st.st_ino;
        s_shm_dev = st.st_dev;
        if (g_shm_fd >= 0) close(g_shm_fd);
        g_shm_fd = shm_fd;  /* keep open so mmap stays alive */
    }

    uint32_t buf_size = pixel_size;
    uint8_t *buf = (uint8_t*)g_shm_map + (buffer_idx * buf_size);

    GLint last_unpack_align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &last_unpack_align);

    int back = 1 - g_tex_idx;

    /* If dimensions changed, delete old texture and reallocate via glTexImage2D */
    bool size_changed = (GLsizei)w != g_tex_w[back] || (GLsizei)h != g_tex_h[back];
    if (size_changed && g_tex[back] != 0) {
        glDeleteTextures(1, &g_tex[back]);
        g_tex[back] = 0;
    }

    if (g_tex[back] == 0) {
        glGenTextures(1, &g_tex[back]);
        glBindTexture(GL_TEXTURE_2D, g_tex[back]);
        if (idk_fn_glPixelStorei) {
            idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        }
        idk_fn_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                            (GLsizei)w, (GLsizei)h, 0,
                            GL_RGBA, GL_UNSIGNED_BYTE, buf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_tex_w[back] = (GLsizei)w;
        g_tex_h[back] = (GLsizei)h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g_tex[back]);
        if (idk_fn_glPixelStorei) {
            idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        }
        idk_fn_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                               (GLsizei)w, (GLsizei)h,
                               GL_RGBA, GL_UNSIGNED_BYTE, buf);
    }

    if (idk_fn_glPixelStorei) {
        idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, last_unpack_align);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    g_tex_idx = back;
    g_has_frame = true;
    g_draw_err_count = 0;  /* fresh texture — reset draw error counter */

    close(shm_fd);

    return g_tex[g_tex_idx];
}

int idk_compositor_render(void) {
    accept_client();
    if (g_client_fd < 0) return -1;

    /* Poll for new frame — keep newest only */
    int processed = 0;

    while (1) {
        struct pollfd pfd = { .fd = g_client_fd, .events = POLLIN };
        int poll_ret = poll(&pfd, 1, 0);
        if (poll_ret <= 0 || !(pfd.revents & POLLIN)) break;

        struct frame_hdr hdr;
        int dmabuf_fd = -1;

        char ctrl_buf[CMSG_SPACE(sizeof(int))];
        char msg_buf[sizeof(struct frame_hdr) + 32];
        struct iovec iov = { .iov_base = msg_buf, .iov_len = sizeof(msg_buf) };
        struct msghdr msgh = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = ctrl_buf,
            .msg_controllen = sizeof(ctrl_buf),
        };

        ssize_t n = recvmsg(g_client_fd, &msgh, MSG_DONTWAIT);
        if (n < (ssize_t)sizeof(struct frame_hdr)) {
            if (n == 0) {
                fprintf(stderr,
                    "[idk-comp] client disconnected (fd=%d), resetting\n",
                    g_client_fd);
                close(g_client_fd);
                g_client_fd = -1;
            }
            break;
        }

        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh); cmsg;
             cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                memcpy(&dmabuf_fd, CMSG_DATA(cmsg), sizeof(int));
                break;
            }
        }

        if (dmabuf_fd < 0) break;

        memcpy(&hdr, msg_buf, sizeof(hdr));

        if (processed > 0) {
            close(dmabuf_fd);
            continue;
        }

        GLuint tex = 0;
        uint8_t frame_type = (hdr.vis_type >> 8) & 0xFF;

        if (frame_type == IDK_FRAME_TYPE_SHM) {
            uint32_t pixel_size = hdr.reserved;
            if (pixel_size == 0) pixel_size = hdr.width * hdr.height * 4;
            tex = shm_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                 pixel_size, hdr.num_planes);
            if (tex == 0) break;
            g_frame_w = hdr.width;
            g_frame_h = hdr.height;
            processed = 1;
            clock_gettime(CLOCK_MONOTONIC, &g_last_frame_ts);
        } else {
            tex = egl_dmabuf_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                         hdr.stride, hdr.format);
            if (tex == 0) {
                IDK_LOG("comp", "DMABUF import failed, telling client to use SHM\n");
                close(dmabuf_fd);
                /* Still send ACK=1 to notify webview → SHM fallback */
                processed = -1;
            } else {
                int back = 1 - g_tex_idx;
                if (g_tex[back]) glDeleteTextures(1, &g_tex[back]);
                g_tex[back] = tex;
                g_tex_idx = back;
                g_has_frame = true;
                g_draw_err_count = 0;
                close(dmabuf_fd);
                g_frame_w = hdr.width;
                g_frame_h = hdr.height;
                processed = 1;
                clock_gettime(CLOCK_MONOTONIC, &g_last_frame_ts);
            }
        }
    }

    if (processed) {
        struct {
            uint8_t ack;
            int32_t w;
            int32_t h;
        } ack_msg = {0};

        ack_msg.ack = (processed < 0) ? 1 : 0;
        if (g_size_pending && processed > 0) {
            IDK_LOG("comp", "sending game size in ACK: %dx%d\n",
                    g_game_w, g_game_h);
            ack_msg.w = g_game_w;
            ack_msg.h = g_game_h;
            g_size_pending = false;
        }

        int flags = fcntl(g_client_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(g_client_fd, F_SETFL, flags | O_NONBLOCK);
        ssize_t _w = write(g_client_fd, &ack_msg, sizeof(ack_msg));
        (void)_w;
        if (flags >= 0) fcntl(g_client_fd, F_SETFL, flags);

        return 0;
    }
    return -1;
}

GLuint egl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h,
                              uint32_t stride, uint32_t format) {
    if (!fn_eglCreateImageKHR) {
        resolve_egl_functions();
        if (!fn_eglCreateImageKHR) {
            IDK_ERR("comp", "EGL dma_buf import not available\n");
            return 0;
        }
    }

    EGLDisplay egl_dpy = EGL_NO_DISPLAY;
    if (fn_eglGetCurrentDisplay) {
        egl_dpy = fn_eglGetCurrentDisplay();
    }
    if (egl_dpy == EGL_NO_DISPLAY) {
        IDK_ERR("comp", "no current EGL display\n");
        return 0;
    }

    uint32_t drm_fmt = format;
    if (drm_fmt == 0) drm_fmt = 0x34324742; /* fallback: BGRA8888 */

    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)w,
        EGL_HEIGHT, (EGLint)h,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)drm_fmt,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_NONE
    };

    EGLImageKHR img = fn_eglCreateImageKHR(
        egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);

    if (img == EGL_NO_IMAGE_KHR) {
        IDK_ERR("comp", "eglCreateImageKHR failed: 0x%04X\n",
                (unsigned int)(fn_eglGetError ? fn_eglGetError() : 0));
        return 0;
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    /* Resolve glEGLImageTargetTexture2DOES — try dlsym from multiple libs
     * then eglGetProcAddress. On GLVND, eglGetProcAddress may return a
     * GLES-only stub that rejects GL_TEXTURE_2D; try libOpenGL.so for
     * the desktop GL dispatch. */
    if (!fn_glEGLImageTargetTexture2DOES) {
        void *lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libOpenGL.so.0", RTLD_NOW);
        if (lib) {
            fn_glEGLImageTargetTexture2DOES =
                (PFN_glEGLImageTargetTexture2DOES_fn)dlsym(lib, "glEGLImageTargetTexture2DOES");
            IDK_LOG("comp", "glEGLImageTargetTexture2DOES dlsym=%p (libOpenGL)\n",
                    (void*)fn_glEGLImageTargetTexture2DOES);
        }
    }
    if (!fn_glEGLImageTargetTexture2DOES && fn_eglGetProcAddress) {
        fn_glEGLImageTargetTexture2DOES = (PFN_glEGLImageTargetTexture2DOES_fn)
            fn_eglGetProcAddress("glEGLImageTargetTexture2DOES");
        IDK_LOG("comp", "glEGLImageTargetTexture2DOES via eglGetProcAddress = %p\n",
                (void*)fn_glEGLImageTargetTexture2DOES);
    }

    GLboolean ok = GL_FALSE;
    GLenum err = GL_NO_ERROR;
    if (fn_glEGLImageTargetTexture2DOES) {
        fn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImage)img);
        ok = GL_TRUE;
        err = glGetError();
    }
    if (err == GL_INVALID_ENUM) {
        IDK_LOG("comp", "glEGLImageTargetTexture2DOES rejected GL_TEXTURE_2D, trying glEGLImageTargetTexStorageEXT\n");
        /* Resolve glEGLImageTargetTexStorageEXT (GL_EXT_EGL_image_storage) */
        if (!fn_glEGLImageTargetTexStorageEXT) {
            void *lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
            if (!lib) lib = dlopen("libOpenGL.so.0", RTLD_NOW);
            if (lib) {
                fn_glEGLImageTargetTexStorageEXT =
                    (PFN_glEGLImageTargetTexStorageEXT_fn)dlsym(lib, "glEGLImageTargetTexStorageEXT");
            }
            if (!fn_glEGLImageTargetTexStorageEXT && fn_eglGetProcAddress) {
                fn_glEGLImageTargetTexStorageEXT =
                    (PFN_glEGLImageTargetTexStorageEXT_fn)fn_eglGetProcAddress("glEGLImageTargetTexStorageEXT");
            }
        }
        if (fn_glEGLImageTargetTexStorageEXT) {
            fn_glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, (GLeglImage)img, NULL);
            err = glGetError();
        }
    }
    if (err != GL_NO_ERROR) {
        IDK_ERR("comp", "EGL image import failed: 0x%04X\n", err);
    }

    if (!ok || err != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        fn_eglDestroyImageKHR(egl_dpy, img);
        return 0;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    fn_eglDestroyImageKHR(egl_dpy, img);

    return tex;
}

void idk_compositor_render_overlay(int x, int y, uint32_t w, uint32_t h) {
    (void)x; (void)y;
    if (g_program == 0) return;

    /* Validate program is still linked */
    if (g_program != 0) {
        GLboolean is_prog = glIsProgram(g_program);
        GLint link_status = 0;
        if (is_prog) glGetProgramiv(g_program, GL_LINK_STATUS, &link_status);
        if (!is_prog || !link_status) {
            static int s_reinit_count = 0;
            s_reinit_count++;
            fprintf(stderr,
                "[idk-comp] program %u invalidated (is_prog=%d link=%d) — re-initializing shaders (attempt %d)\n",
                g_program, (int)is_prog, (int)link_status, s_reinit_count);
            /* Clear g_program so init creates a fresh one.
             * Don't call glDeleteProgram — the ID may belong to the host. */
            g_program = 0;
            if (idk_compositor_init_gl() != 0) {
                IDK_ERR("comp", "shader re-init failed — skipping frame\n");
                return;
            }
            IDK_LOG("comp", "shader re-init OK, new g_program=%u\n", g_program);
        }
    }

    /* Global stale frame timeout — clear overlay if no frame for 1s
       (handles webview crash, driver crash, resize hang, etc.) */
    if (g_has_frame) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - g_last_frame_ts.tv_sec)
                       + (now.tv_nsec - g_last_frame_ts.tv_nsec) / 1e9;
        if (elapsed > 1.0) {
            IDK_LOG("comp", "stale frame cleared (%.0fms since last frame)\n", elapsed * 1000);
            g_has_frame = false;
        }
    }

    if (!g_has_frame || g_tex[g_tex_idx] == 0) {
        return;
    }

    GLint fb_w = (GLint)w, fb_h = (GLint)h;
    {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        fb_w = vp[2];
        fb_h = vp[3];
    }
    if (fb_w <= 0 || fb_h <= 0) return;

    GLenum last_active_texture; glGetIntegerv(GL_ACTIVE_TEXTURE, (GLint*)&last_active_texture);
    glActiveTexture(GL_TEXTURE0);
    GLint last_program; glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
    GLint last_texture; glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);

    GLint last_sampler = 0;
    if (!g_is_gles && g_gl_version >= 330)
        glGetIntegerv(GL_SAMPLER_BINDING, &last_sampler);

    GLint last_array_buffer; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    GLint last_element_array_buffer = 0;
    if (g_gl_version >= 300)
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_array_buffer);

    GLint last_vertex_array_object = 0;
    if (g_gl_version >= 300)
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array_object);

    GLint last_polygon_mode[2] = {GL_FILL, GL_FILL};
    if (!g_is_gles && g_gl_version >= 200)
        glGetIntegerv(GL_POLYGON_MODE, last_polygon_mode);

    GLint last_viewport[4]; glGetIntegerv(GL_VIEWPORT, last_viewport);
    GLint last_scissor_box[4]; glGetIntegerv(GL_SCISSOR_BOX, last_scissor_box);

    GLint last_color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetIntegerv(GL_COLOR_WRITEMASK, last_color_mask);

    GLenum last_blend_src_rgb; glGetIntegerv(GL_BLEND_SRC_RGB, (GLint*)&last_blend_src_rgb);
    GLenum last_blend_dst_rgb; glGetIntegerv(GL_BLEND_DST_RGB, (GLint*)&last_blend_dst_rgb);
    GLenum last_blend_src_alpha; glGetIntegerv(GL_BLEND_SRC_ALPHA, (GLint*)&last_blend_src_alpha);
    GLenum last_blend_dst_alpha; glGetIntegerv(GL_BLEND_DST_ALPHA, (GLint*)&last_blend_dst_alpha);
    GLenum last_blend_equation_rgb; glGetIntegerv(GL_BLEND_EQUATION_RGB, (GLint*)&last_blend_equation_rgb);
    GLenum last_blend_equation_alpha; glGetIntegerv(GL_BLEND_EQUATION_ALPHA, (GLint*)&last_blend_equation_alpha);
    GLboolean last_enable_blend = glIsEnabled(GL_BLEND);
    GLboolean last_enable_cull_face = glIsEnabled(GL_CULL_FACE);
    GLboolean last_enable_depth_test = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_enable_stencil_test = glIsEnabled(GL_STENCIL_TEST);
    GLboolean last_enable_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean last_srgb_enabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLboolean last_enable_primitive_restart = (!g_is_gles && g_gl_version >= 310) ? glIsEnabled(GL_PRIMITIVE_RESTART) : GL_FALSE;

    GLint last_fbo = -1;
    if (g_gl_version >= 300)
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &last_fbo);

    GLint last_draw_buffer = GL_BACK;
    glGetIntegerv(GL_DRAW_BUFFER, &last_draw_buffer);

    while (glGetError() != GL_NO_ERROR) {}

    #define GLCHECK(label) do { \
        if (g_draw_err_count == 1) { \
            GLenum _e = glGetError(); \
            if (_e != GL_NO_ERROR) \
                IDK_ERR("comp", "GL setup error @ %s: 0x%x\n", label, _e); \
        } \
    } while(0)

    glEnable(GL_BLEND);                GLCHECK("glEnable(GL_BLEND)");
    glBlendEquation(GL_FUNC_ADD);      GLCHECK("glBlendEquation");
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                                       GLCHECK("glBlendFuncSeparate");
    glDisable(GL_CULL_FACE);           GLCHECK("glDisable(GL_CULL_FACE)");
    glDisable(GL_DEPTH_TEST);          GLCHECK("glDisable(GL_DEPTH_TEST)");
    glDisable(GL_STENCIL_TEST);        GLCHECK("glDisable(GL_STENCIL_TEST)");
    glEnable(GL_SCISSOR_TEST);         GLCHECK("glEnable(GL_SCISSOR_TEST)");
    glScissor(0, 0, fb_w, fb_h);       GLCHECK("glScissor");
    if (g_gl_version >= 300) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                                       GLCHECK("glBindFramebuffer");
    }
    glDisable(GL_FRAMEBUFFER_SRGB);    GLCHECK("glDisable(GL_FRAMEBUFFER_SRGB)");
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                                       GLCHECK("glColorMask");

    if (!g_is_gles) {
        if (g_gl_version >= 200) { glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                                       GLCHECK("glPolygonMode"); }
        if (g_gl_version >= 310) { glDisable(GL_PRIMITIVE_RESTART);
                                       GLCHECK("glDisable(GL_PRIMITIVE_RESTART)"); }
    }

    glViewport(0, 0, fb_w, fb_h);      GLCHECK("glViewport");
    glDrawBuffer(GL_BACK);             GLCHECK("glDrawBuffer");

    GLuint vertex_array_object = 0;
    if (g_gl_version >= 300)
        glGenVertexArrays(1, &vertex_array_object);
    if (vertex_array_object) {
        glBindVertexArray(vertex_array_object);
                                       GLCHECK("glBindVertexArray");
    }

    if (g_draw_err_count == 1) {
        GLuint cur_tex = g_tex[g_tex_idx];
        GLboolean tex_valid = cur_tex ? glIsTexture(cur_tex) : GL_FALSE;
        GLint prog_link = 0;
        glGetProgramiv(g_program, GL_LINK_STATUS, &prog_link);
        fprintf(stderr,
            "[idk-comp] pre-draw diag: tex[%d]=%u valid=%d prog=%u link=%d "
            "frame=%ux%u fb=%dx%d\n",
            g_tex_idx, cur_tex, (int)tex_valid, g_program, (int)prog_link,
            g_frame_w, g_frame_h, fb_w, fb_h);
    }

    glUseProgram(g_program);           GLCHECK("glUseProgram");
    GLint loc = glGetUniformLocation(g_program, "u_texture");
                                       GLCHECK("glGetUniformLocation");
    glUniform1i(loc, 0);               GLCHECK("glUniform1i");
    glActiveTexture(GL_TEXTURE0);      GLCHECK("glActiveTexture");
    glBindTexture(GL_TEXTURE_2D, g_tex[g_tex_idx]);
                                       GLCHECK("glBindTexture");
    #undef GLCHECK

    glDrawArrays(GL_TRIANGLES, 0, 6);
    {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            g_draw_err_count++;
            /* Throttle: log first 5, then every 300th */
            if (g_draw_err_count <= 5 || g_draw_err_count % 300 == 0) {
                fprintf(stderr,
                    "[idk-comp] GL error after draw: 0x%x (tex[%d]=%u fb=%dx%d frame=%ux%u, occurrence %d)\n",
                    err, g_tex_idx, g_tex[g_tex_idx], fb_w, fb_h,
                    g_frame_w, g_frame_h, g_draw_err_count);
            }
            /* Invalidate texture after 5 consecutive errors */
            if (g_draw_err_count == 5) {
                GLuint dead = g_tex[g_tex_idx];
                fprintf(stderr,
                    "[idk-comp] tex[%d]=%u failing repeatedly — deleting + marking invalid\n",
                    g_tex_idx, dead);
                if (dead > 0) glDeleteTextures(1, &dead);
                g_tex[g_tex_idx] = 0;
                g_has_frame = false;
            }
        }
        /* Reset only on new frame upload in shm_to_texture / dmabuf path */
    }
    GLuint cur = g_tex[g_tex_idx];
    if (cur > 0 && !glIsTexture(cur)) {
        IDK_ERR("comp", "WARNING: tex[%d] (%u) is not a valid texture!\n", g_tex_idx, cur);
        g_tex[g_tex_idx] = 0;
        g_has_frame = false;
    }

    if (vertex_array_object)
        glDeleteVertexArrays(1, &vertex_array_object);

    glUseProgram(last_program);
    glBindTexture(GL_TEXTURE_2D, last_texture);

    if (!g_is_gles && g_gl_version >= 330)
        glBindSampler(0, (GLuint)last_sampler);

    glActiveTexture(last_active_texture);

    if (g_gl_version >= 300)
        glBindVertexArray((GLuint)last_vertex_array_object);

    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
    if (g_gl_version >= 300 && last_element_array_buffer >= 0)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)last_element_array_buffer);
    glBlendEquationSeparate(last_blend_equation_rgb, last_blend_equation_alpha);
    glBlendFuncSeparate(last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha);
    if (last_enable_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (last_enable_cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (last_enable_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_enable_stencil_test) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (last_enable_scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (!g_is_gles && g_gl_version >= 310) { if (last_enable_primitive_restart) glEnable(GL_PRIMITIVE_RESTART); }
    if (!g_is_gles && g_gl_version >= 200)
        glPolygonMode(GL_FRONT_AND_BACK, (GLenum)last_polygon_mode[0]);

    glViewport(last_viewport[0], last_viewport[1], (GLsizei)last_viewport[2], (GLsizei)last_viewport[3]);
    glScissor(last_scissor_box[0], last_scissor_box[1], (GLsizei)last_scissor_box[2], (GLsizei)last_scissor_box[3]);

    if (last_srgb_enabled) glEnable(GL_FRAMEBUFFER_SRGB);
    glDrawBuffer((GLenum)last_draw_buffer);
    if (last_fbo >= 0)
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)last_fbo);

    glColorMask((GLboolean)last_color_mask[0], (GLboolean)last_color_mask[1],
                (GLboolean)last_color_mask[2], (GLboolean)last_color_mask[3]);
}

int idk_compositor_has_overlay(void) {
    return g_has_frame;
}

int idk_compositor_set_overlay(int dmabuf_fd, uint32_t w, uint32_t h,
                                 uint32_t stride, uint32_t format) {
    (void)stride; (void)format;
    /* This is a legacy API — use idk_compositor_render() instead */
    /* For backward compat, just store and let render() pick it up */
    if (dmabuf_fd >= 0) {
        g_dmabuf_fd = dmabuf_fd;
        g_frame_w = w;
        g_frame_h = h;
        return 0;
    }
    return -1;
}

void idk_compositor_shutdown(void) {
    if (g_shm_map) {
        munmap(g_shm_map, g_shm_map_size);
        g_shm_map = NULL;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }

    if (g_dmabuf_fd >= 0) {
        close(g_dmabuf_fd);
        g_dmabuf_fd = -1;
    }

    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
    }

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    /* Remove socket file */
    if (g_sock_path[0]) {
        unlink(g_sock_path);
    }

    /* GL cleanup skipped — OS reclaims on exit */
    g_program = 0;
    g_tex[0] = g_tex[1] = 0;
    g_has_frame = false;
    IDK_LOG("comp", "Shut down\n");
}

/* ── GL resource initialization ────────────────────────────────────── */

/* Init shaders and GL resources */
int idk_compositor_init_gl(void) {
    if (idk_gl_loader_init() != 0) {
        IDK_ERR("comp", "GL loader init failed — cannot init GL resources\n");
        return -1;
    }

    void *libgl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!libgl) libgl = dlopen("libGL.so.1", RTLD_NOW);
    if (!libgl) libgl = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!libgl) libgl = dlopen("libOpenGL.so.0", RTLD_NOW);
    if (!libgl) libgl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_NOLOAD);
    if (!libgl) libgl = dlopen("libGLESv2.so.2", RTLD_NOW);
    if (!libgl) libgl = dlopen("libGL.so", RTLD_NOW);
    if (libgl) {
        fn_glEGLImageTargetTexture2DOES =
            (PFN_glEGLImageTargetTexture2DOES_fn)dlsym(libgl, "glEGLImageTargetTexture2DOES");
        IDK_LOG("comp", "glEGLImageTargetTexture2DOES dlsym=%p (libgl)\n",
                (void *)fn_glEGLImageTargetTexture2DOES);
    }
    g_program = idk_shader_loader_init(&g_gl_version, &g_is_gles);
    if (g_program == 0) {
        IDK_ERR("comp", "Shader init failed — cannot init GL resources\n");
        return -1;
    }

    IDK_LOG("comp", "GL compositor ready (program=%u)\n", g_program);
    return 0;
}
