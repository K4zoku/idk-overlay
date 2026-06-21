/*
 * compositor.c — Wayland EGL compositor for overlay frames
 *
 * Receives dmabuf frames from webview, converts to GL textures via
 * EGL DMA-BUF import, and renders fullscreen quads on top of the
 * game's EGL framebuffer.
 *
 * This is the SAME approach used by Steam/Discord overlays on Wayland:
 * render additional content in the same EGL context before swap.
 *
 * Flow:
 *   1. Background thread receives dmabuf from webview socket
 *   2. Converts to EGLImage → GL texture
 *   3. EGL swap hook renders overlay quad before calling original swap
 *   4. Game + overlay are presented together
 */

#define _GNU_SOURCE

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

#include "idk_gl_loader.h"   /* GL types + function pointer redirects */
#include "compositor.h"
#include "overlay_shader_embed.h"
#include "idk_ipc.h"

/* ── Frame header from webview ─────────────────────────────────────── */
/*
 * Wire format (matches idk_client_t::build_frame_hdr):
 *   fields[0] = width
 *   fields[1] = height
 *   fields[2] = x position (overloaded "stride")
 *   fields[3] = y position (overloaded "format")
 *   fields[4] = overlay ID (overloaded "num_planes")
 *   fields[5] = pixel byte size (overloaded "pid") — used by SHM mode
 *   fields[6] = visibility (low byte) + frame type (high byte)
 *               type: 0 = DMABUF, 1 = SHM
 *   fields[7] = checksum
 */
struct frame_hdr {
    uint32_t width;
    uint32_t height;
    uint32_t stride;       /* actually X position */
    uint32_t format;       /* actually Y position */
    uint32_t num_planes;   /* actually overlay ID */
    uint32_t reserved;     /* actually pixel byte size (SHM) */
    uint32_t vis_type;     /* visibility (low byte) + frame type (high byte) */
    uint32_t checksum;
};

#define IDK_FRAME_TYPE_DMABUF  0
#define IDK_FRAME_TYPE_SHM     1

/* ── EGL types (opaque — we don't need full EGL headers) ────────────────── */

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
#define EGL_DRM_BUFFER_FORMAT_MESA     0x31DD
#define EGL_DRM_BUFFER_STRIDE_MESA     0x31DE

/* ── EGL function pointers (resolved at runtime via dlsym) ──────────────── */

typedef EGLDisplay (*PFN_eglGetDisplay_fn)(EGLNativeDisplayType);
typedef EGLint (*PFN_eglGetError_fn)(void);
typedef EGLImageKHR (*PFN_eglCreateImageKHR_fn)(EGLDisplay dpy, EGLContext ctx,
                                                 EGLenum target,
                                                 void *buffer,
                                                 const EGLint *attrs);
typedef EGLBoolean (*PFN_eglDestroyImageKHR_fn)(EGLDisplay dpy, EGLImageKHR image);

static PFN_eglGetDisplay_fn       fn_eglGetDisplay       = NULL;
static PFN_eglGetError_fn         fn_eglGetError         = NULL;
static PFN_eglCreateImageKHR_fn   fn_eglCreateImageKHR   = NULL;
static PFN_eglDestroyImageKHR_fn  fn_eglDestroyImageKHR  = NULL;

/* Resolve EGL function pointers from libEGL.so.1 (RTLD_NOLOAD — reuse
 * the lib already loaded by the target process). */
static void resolve_egl_functions(void) {
    if (fn_eglGetDisplay) return;
    void *lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libEGL.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libEGL.so", RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "[idk-comp] dlopen libEGL failed: %s\n", dlerror());
        return;
    }
    fn_eglGetDisplay       = (PFN_eglGetDisplay_fn)     dlsym(lib, "eglGetDisplay");
    fn_eglGetError         = (PFN_eglGetError_fn)       dlsym(lib, "eglGetError");
    fn_eglCreateImageKHR   = (PFN_eglCreateImageKHR_fn) dlsym(lib, "eglCreateImageKHR");
    fn_eglDestroyImageKHR  = (PFN_eglDestroyImageKHR_fn)dlsym(lib, "eglDestroyImageKHR");
    fprintf(stderr, "[idk-comp] EGL functions: eglGetDisplay=%p eglCreateImageKHR=%p\n",
            (void*)fn_eglGetDisplay, (void*)fn_eglCreateImageKHR);
}

/* ── GL state ──────────────────────────────────────────────────────── */

static GLuint g_program = 0;


/* Double-buffered overlay textures — upload to back while drawing from front */
static GLuint g_tex[2] = {0, 0};
static int g_tex_idx = 0;   /* index of current display texture (0 or 1) */
static bool g_has_frame = false;

/* GL error counter for render_overlay — counts consecutive draw errors.
 * Reset to 0 on successful frame upload (shm_to_texture / dmabuf path)
 * so each new texture gets a fresh chance. When it hits 5, the current
 * texture is marked invalid (g_tex[g_tex_idx]=0, g_has_frame=false) to
 * stop spam at its source.
 *
 * IMPORTANT: must be file-scope (not function-scope static) so it can be
 * reset from shm_to_texture / dmabuf path. With function-scope only, the
 * counter never resets on new frame upload → climbs to 39000+ on a
 * persistently-bad texture, with "if (count == 5)" only firing once. */
static int g_draw_err_count = 0;

/* Persistent SHM mapping (kept between frames for zero-copy reads) */
static int    g_shm_fd = -1;
static void  *g_shm_map = NULL;
static size_t g_shm_map_size = 0;

/* GL version detection (MangoHud-style) */
static bool g_is_gles = false;
static int g_gl_version = 0;  /* major*100 + minor*10, e.g. 330 for GL 3.3 */

/* DMABUF fd — kept open until next frame replaces it */
static int g_dmabuf_fd = -1;
static uint32_t g_frame_w = 0;
static uint32_t g_frame_h = 0;

/* GL function pointers for GL 2.0+ (resolved at runtime) */
typedef void* GLeglImage;
typedef void (*PFN_glEGLImageTargetTexture2DOES_fn)(GLenum target, GLeglImage image);
static PFN_glEGLImageTargetTexture2DOES_fn fn_glEGLImageTargetTexture2DOES = NULL;

/* Forward declaration */
static GLuint egl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h,
                                    uint32_t stride, uint32_t format);

/* ── Webview socket receiver (server) ──────────────────────────────── */

static int g_listen_fd = -1;
static int g_client_fd = -1;
static char g_sock_path[512];

/**
 * Start listening for overlay frames from webview clients.
 * Must be called from main thread (creates/binds socket).
 * Returns 0 on success.
 * Uses IDK_SOCKET env var if set, falls back to /tmp/idk-overlay.
 */
int idk_compositor_init(void) {
    const char *env_sock = getenv("IDK_SOCKET");
    if (env_sock && env_sock[0] != '\0') {
        snprintf(g_sock_path, sizeof(g_sock_path), "%s", env_sock);
    } else {
        snprintf(g_sock_path, sizeof(g_sock_path), "/tmp/idk-overlay");
    }

    /* Check if a previous instance is still alive.
     * If the socket exists AND we can connect, another instance owns it.
     * We're a spare — exit immediately. */
    {
        struct stat st;
        if (stat(g_sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un addr = { .sun_family = AF_UNIX };
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_sock_path);
            if (connect(test_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                fprintf(stderr, "[idk-comp] Another instance is alive — exiting\n");
                close(test_fd);
                exit(0);
            }
            close(test_fd);
        }
    }

    /* Remove stale socket */
    unlink(g_sock_path);

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        fprintf(stderr, "[idk-comp] socket() failed: %s\n", strerror(errno));
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
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_sock_path);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* Retry: stale socket may have been re-created between unlink and bind */
        unlink(g_sock_path);
        if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            if (errno == EADDRINUSE) {
                /* Another instance owns this socket — we're a spare, exit */
                fprintf(stderr, "[idk-comp] Another instance owns the socket, exiting\n");
                exit(0);
            }
            fprintf(stderr, "[idk-comp] bind() failed: %s\n", strerror(errno));
            close(g_listen_fd);
            g_listen_fd = -1;
            return -1;
        }
    }

    if (listen(g_listen_fd, 4) < 0) {
        fprintf(stderr, "[idk-comp] listen() failed: %s\n", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    /* Accept first client (non-blocking — return -1 if none yet) */
    g_client_fd = accept(g_listen_fd, NULL, NULL);
    if (g_client_fd < 0) {
        fprintf(stderr, "[idk-comp] No client yet (fd=-1, will retry next frame, err=%s)\n",
                strerror(errno));
        g_client_fd = -1;
    }

    /* Init GL resources (will be used in GL context thread) */
    g_has_frame = false;

    fprintf(stderr, "[idk-comp] Listening on %s, waiting for webview client...\n", g_sock_path);
    return 0;
}

/**
 * Accept a new client connection (called from render loop).
 * Non-blocking.
 */
static int accept_client(void) {
    if (g_client_fd >= 0) return 0; /* already have a client */
    if (g_listen_fd < 0) return -1;

    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int client = accept(g_listen_fd, (struct sockaddr *)&addr, &addrlen);
    if (client >= 0) {
        fprintf(stderr, "[idk-comp] Client connected (fd=%d)\n", client);
        g_client_fd = client;
    }
    return 0;
}

/**
 * Called from EGL swap hook (before original swap) to receive and render
 * a new overlay frame. Non-blocking.
 *
 * Returns 0 if frame rendered, -1 if no frame.
 */
/*
 * Convert SHM pixel data to GL texture via glTex(Sub)Image2D.
 * Uses a persistent mmap and persistent GL texture — does not
 * delete/recreate per frame (avoids flickering from double-buffer
 * offset mismatch).
 *
 * Pixel format: RGBA8888 (matches Qt's QImage::Format_RGBA8888).
 * buffer_idx: which half of the double-buffered SHM has fresh data.
 */
static GLuint shm_to_texture(int shm_fd, uint32_t w, uint32_t h,
                              uint32_t pixel_size, uint32_t buffer_idx) {
    if (!idk_fn_glTexImage2D) {
        fprintf(stderr, "[idk-comp] glTexImage2D not resolved\n");
        return 0;
    }

    /* ── Validate dimensions BEFORE glTexImage2D / glTexSubImage2D ─────
     * Both calls with width=0 or height=0 return GL_INVALID_VALUE (0x501).
     * Likely root cause of the flaky "GL error after draw: 0x501" — on the
     * first eglSwapBuffers, eglQuerySurface may return 0×0 before the
     * surface is fully realized, and the client may send a frame with the
     * same 0×0 dimensions before ACK flow control kicks in. Also catches
     * pixel_size < w*h*4 (SHM buffer smaller than expected RGBA payload),
     * which would make glTexSubImage2D read past the mmap region.
     */
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
    /* pixel_size > expected is OK — driver reads expected bytes, extra is
     * padding (e.g. row alignment). Only pixel_size < expected is rejected. */

    /* Check if this is a new memfd (via inode comparison) */
    struct stat st;
    if (fstat(shm_fd, &st) < 0) {
        fprintf(stderr, "[idk-comp] SHM fstat failed: %s\n", strerror(errno));
        return 0;
    }
    static ino_t s_shm_ino = 0;
    static dev_t s_shm_dev = 0;

    if (st.st_ino != s_shm_ino || st.st_dev != s_shm_dev) {
        /* New memfd — remap */
        if (g_shm_map) {
            munmap(g_shm_map, g_shm_map_size);
            g_shm_map = NULL;
        }
        off_t total = lseek(shm_fd, 0, SEEK_END);
        if (total <= 0) total = (off_t)pixel_size;

        g_shm_map_size = (size_t)total;
        g_shm_map = mmap(NULL, g_shm_map_size, PROT_READ, MAP_SHARED, shm_fd, 0);
        if (g_shm_map == MAP_FAILED) {
            fprintf(stderr, "[idk-comp] SHM mmap failed: %s\n", strerror(errno));
            g_shm_map = NULL;
            return 0;
        }
        s_shm_ino = st.st_ino;
        s_shm_dev = st.st_dev;
        if (g_shm_fd >= 0) close(g_shm_fd);
        g_shm_fd = shm_fd;  /* keep open so mmap stays alive */
    }

    /* Compute buffer offset within the double-buffered memfd */
    uint32_t buf_size = pixel_size;
    uint8_t *buf = (uint8_t*)g_shm_map + (buffer_idx * buf_size);

    /* Upload to the BACK texture (the one NOT currently displayed),
     * so the FRONT texture remains valid for concurrent drawing. */
    int back = 1 - g_tex_idx;
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
    } else {
        glBindTexture(GL_TEXTURE_2D, g_tex[back]);
        idk_fn_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                               (GLsizei)w, (GLsizei)h,
                               GL_RGBA, GL_UNSIGNED_BYTE, buf);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Swap: the newly uploaded texture becomes the display texture */
    g_tex_idx = back;
    g_has_frame = true;
    g_draw_err_count = 0;  /* fresh texture — reset draw error counter */

    /* Close the received fd — if it's the same memfd, our persistent
     * mmap keeps it alive. If it's a new one, the mmap above references
     * it. Either way, the fd we received is safe to close. */
    close(shm_fd);

    return g_tex[g_tex_idx];
}

int idk_compositor_render(void) {
    /* Try to accept client connection if none yet */
    accept_client();

    if (g_client_fd < 0) return -1;

    struct frame_hdr hdr;
    int dmabuf_fd = -1;

    /* Non-blocking poll for new frame */
    struct pollfd pfd = { .fd = g_client_fd, .events = POLLIN };
    int poll_ret = poll(&pfd, 1, 0);
    if (poll_ret <= 0) {
        /* Log once every ~60 frames (1/sec at 60fps) */
        static int poll_skip_count = 0;
        if (++poll_skip_count >= 60) {
            poll_skip_count = 0;
            fprintf(stderr, "[idk-comp] poll: no data (fd=%d, ret=%d)\n",
                    g_client_fd, poll_ret);
        }
        return -1; /* no frame ready */
    }
    if (!(pfd.revents & POLLIN)) {
        fprintf(stderr, "[idk-comp] poll revents=0x%x (no POLLIN)\n", pfd.revents);
        return -1;
    }

    fprintf(stderr, "[idk-comp] poll OK — data available\n");

    /* Receive frame header + fd */
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
    fprintf(stderr, "[idk-comp] recvmsg returned %zd (need %zu)\n",
            n, sizeof(struct frame_hdr));
    if (n < (ssize_t)sizeof(struct frame_hdr)) {
        fprintf(stderr, "[idk-comp] recvmsg too short — skipping\n");

        /* recvmsg() returning exactly 0 means the client closed the socket
         * (clean EOF). POLLIN keeps firing on a closed socket forever, so
         * without handling this we end up in an infinite poll→recv(0)→
         * skip→draw(stale texture)→0x501 loop. Close and reset so
         * accept_client() can pick up a new connection. */
        if (n == 0) {
            fprintf(stderr,
                "[idk-comp] client disconnected (fd=%d), resetting\n",
                g_client_fd);
            close(g_client_fd);
            g_client_fd = -1;
            /* Keep g_has_frame and g_tex[g_tex_idx] — the last good frame
             * can still be drawn until a new client connects. Reset only
             * if you want a clean slate; leaving it lets the overlay
             * persist across reconnects. */
        }
        /* n < 0 with errno=EAGAIN/EWOULDBLOCK is normal — no frame yet.
         * n > 0 but < header size is a malformed/partial frame — skip. */
        return -1;
    }

    /* Extract dmabuf fd from SCM_RIGHTS */
    dmabuf_fd = -1;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh); cmsg;
         cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(&dmabuf_fd, CMSG_DATA(cmsg), sizeof(int));
            break;
        }
    }

    if (dmabuf_fd < 0) {
        fprintf(stderr, "[idk-comp] no fd in SCM_RIGHTS\n");
        return -1;
    }

    memcpy(&hdr, msg_buf, sizeof(hdr));

    fprintf(stderr, "[idk-comp] frame: %ux%u type=%u fd=%d\n",
            hdr.width, hdr.height, (hdr.vis_type >> 8) & 0xFF, dmabuf_fd);

    /* Upload to GL texture.
     *
     * Two paths selected by frame type in vis_type high byte:
     *   1. SHM mode (type == 1): fd is a memfd containing raw RGBA8888
     *      pixel data. Use glTexImage2D to upload.
     *   2. DMABUF mode (type == 0): fd is a real DMA-BUF. Use
     *      egl_dmabuf_to_texture via EGL_EXT_image_dma_buf_import.
     *      If that fails, fall back to SHM (auto-fallback).
     */
    GLuint tex = 0;
    uint8_t frame_type = (hdr.vis_type >> 8) & 0xFF;

    if (frame_type == IDK_FRAME_TYPE_SHM) {
        /* SHM path: 'reserved' field = pixel byte size per buffer */
        uint32_t pixel_size = hdr.reserved;
        if (pixel_size == 0) pixel_size = hdr.width * hdr.height * 4;
        /* hdr.num_planes encodes the double-buffer index from the Qt client */
        tex = shm_to_texture(dmabuf_fd, hdr.width, hdr.height,
                             pixel_size, hdr.num_planes);
        if (tex == 0) {
            fprintf(stderr, "[idk-comp] SHM texture upload failed\n");
            return -1;
        }
    } else {
        /* DMABUF path — try EGL import first */
        tex = egl_dmabuf_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                     hdr.stride, hdr.format);
        if (tex == 0) {
            /* Fallback: maybe this is actually a memfd, not real DMABUF.
             * Try SHM upload with default pixel size. */
            uint32_t pixel_size = hdr.width * hdr.height * 4;
            fprintf(stderr, "[idk-comp] DMABUF import failed, trying SHM fallback\n");
            tex = shm_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                 pixel_size, hdr.num_planes);
            if (tex == 0) {
                return -1;
            }
        }
    }

    /* For DMABUF path, double-buffer: replace back texture then swap. */
    if (frame_type != IDK_FRAME_TYPE_SHM) {
        int back = 1 - g_tex_idx;
        if (g_tex[back]) glDeleteTextures(1, &g_tex[back]);
        g_tex[back] = tex;
        g_tex_idx = back;
        g_has_frame = true;
        g_draw_err_count = 0;  /* fresh texture — reset draw error counter */
        close(dmabuf_fd);
    }
    g_frame_w = hdr.width;
    g_frame_h = hdr.height;

    fprintf(stderr, "[idk-comp] texture uploaded: tex[%d]=%u %ux%u pixel_size=%u\n",
            g_tex_idx, g_tex[g_tex_idx], hdr.width, hdr.height, hdr.reserved);

    /* Send ACK to client — flow control: client waits for this before
     * rendering/sending the next frame. This syncs webview frame rate
     * to the game's swap rate, preventing SHM buffer races. */
    {
        char ack = 0;
        if (write(g_client_fd, &ack, 1) < 0) {
            fprintf(stderr, "[idk-comp] ACK write failed: %s\n", strerror(errno));
        }
    }

    /* Close the dmabuf fd — GL texture keeps a reference */
    close(dmabuf_fd);

    return 0;
}

/**
 * Convert dmabuf to GL texture via EGL.
 * Uses EGL_EXT_image_dma_buf_import to import dmabuf directly.
 * No DRM header dependency — format is handled by EGL driver.
 */
GLuint egl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h,
                              uint32_t stride, uint32_t format) {
    if (!fn_eglCreateImageKHR) {
        resolve_egl_functions();
        if (!fn_eglCreateImageKHR) {
            fprintf(stderr, "[idk-comp] EGL dma_buf import not available\n");
            return 0;
        }
    }

    /* Get EGL display */
    EGLDisplay egl_dpy = EGL_NO_DISPLAY;
    if (fn_eglGetDisplay) {
        egl_dpy = fn_eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (egl_dpy == EGL_NO_DISPLAY) {
            egl_dpy = fn_eglGetDisplay((EGLNativeDisplayType)0);
        }
    }
    if (egl_dpy == EGL_NO_DISPLAY) return 0;

    /* Format from IDK → EGL DRM format (both use same fourcc)
     * 0x34324742 = 'BG24' = DRM_FORMAT_BGRA8888
     * 0x34325258 = 'XR24' = DRM_FORMAT_XRGB8888
     */
    uint32_t drm_fmt = 0x34324742; /* default: BGRA8888 */
    if (format == 0x34325258) {
        drm_fmt = 0x34325258; /* XR24 = XRGB8888 */
    }

    /* Create EGLImage from DMA-BUF */
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)w,
        EGL_HEIGHT, (EGLint)h,
        EGL_LINUX_DMA_BUF_EXT, dmabuf_fd,
        EGL_DRM_BUFFER_FORMAT_MESA, drm_fmt,
        EGL_DRM_BUFFER_STRIDE_MESA, (EGLint)stride,
        EGL_NONE
    };

    EGLImageKHR img = fn_eglCreateImageKHR(
        egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);

    if (img == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "[idk-comp] eglCreateImageKHR failed: 0x%04X\n",
                (unsigned int)(fn_eglGetError ? fn_eglGetError() : 0));
        return 0;
    }

    /* Bind to GL texture */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (fn_glEGLImageTargetTexture2DOES) {
        fn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImage)img);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Destroy EGL image — GL texture keeps reference */
    fn_eglDestroyImageKHR(egl_dpy, img);

    fprintf(stderr, "[idk-comp] Uploaded overlay %dx%d as GL tex %u\n",
            w, h, tex);
    return tex;
}

/**
 * Render the current overlay texture as a fullscreen quad.
 * Call this from the EGL swap hook (before calling original swap).
 *
 * Adapted from imgoverlay/imgui_impl_opengl3 — uses GLES2-compatible
 * vertex attribute setup (no VAO, manual glEnableVertexAttribArray).
 */
void idk_compositor_render_overlay(int x, int y, uint32_t w, uint32_t h) {
    if (g_program == 0) return;

    /* ── Don't draw if we have no valid frame ──────────────────────────
     * Without this guard, when no client has sent a frame yet (or the
     * last frame was rejected by shm_to_texture's validation), we'd
     * call glBindTexture(0) + glDrawArrays → GL_INVALID_OPERATION or
     * GL_INVALID_VALUE every frame → 0x501 spam in the log.
     * The original code only checked g_program, not g_has_frame. */
    if (!g_has_frame || g_tex[g_tex_idx] == 0) {
        return;
    }

    /* Use framebuffer dimensions for viewport */
    GLint fb_w = (GLint)w, fb_h = (GLint)h;
    {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        fb_w = vp[2];
        fb_h = vp[3];
    }
    if (fb_w <= 0 || fb_h <= 0) return;

    static int s_render_debug = 0;
    int dbg = s_render_debug++;
    if (dbg < 5 || dbg % 120 == 0) {
        fprintf(stderr, "[idk-comp] render_overlay: fb=%dx%d prog=%u\n",
                fb_w, fb_h, g_program);
    }

    /* ── Save GL state (MangoHud-style) ── */
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
    {
        static int dbg = 0;
        if (dbg++ < 3)
            fprintf(stderr, "[idk-comp] draw_buffer=0x%x\n", last_draw_buffer);
    }

    /* Clear any stale GL errors before our draw */
    while (glGetError() != GL_NO_ERROR) {}

    /* ── Setup render state (MangoHud-style) ──
     * Each call is followed by an error check ON THE FIRST FAILED DRAW
     * (g_draw_err_count == 1) so we can identify exactly which GL call
     * sets the error that post-draw glGetError picks up. After the first
     * diagnosis, the checks are skipped (g_draw_err_count > 1) to avoid
     * per-frame overhead. */
    #define GLCHECK(label) do { \
        if (g_draw_err_count == 1) { \
            GLenum _e = glGetError(); \
            if (_e != GL_NO_ERROR) \
                fprintf(stderr, "[idk-comp] GL setup error @ %s: 0x%x\n", label, _e); \
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
    /* Bind to default framebuffer (FBO 0) to ensure overlay renders to back buffer */
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

    /* Set viewport to full framebuffer (MangoHud overrides viewport) */
    glViewport(0, 0, fb_w, fb_h);      GLCHECK("glViewport");
    /* Ensure shader output goes to back buffer */
    glDrawBuffer(GL_BACK);             GLCHECK("glDrawBuffer");

    /* Create temporary VAO (MangoHud creates one per frame if GL >= 3.0) */
    GLuint vertex_array_object = 0;
    if (g_gl_version >= 300)
        glGenVertexArrays(1, &vertex_array_object);
    if (vertex_array_object) {
        glBindVertexArray(vertex_array_object);
                                       GLCHECK("glBindVertexArray");
    }

    /* Debug: track which buffer we're using */
    static int s_frame_counter = 0;
    s_frame_counter++;
    if (s_frame_counter % 60 == 0) {
        fprintf(stderr, "[idk-comp] render: tex[%d]=%u has_frame=%d fb=%dx%d\n",
                g_tex_idx, g_tex[g_tex_idx], g_has_frame, fb_w, fb_h);
    }

    /* On first failure, log texture validity + program status — these are
     * the most common 0x501 sources when no setup call above errors. */
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
        /* g_draw_err_count is file-scope — reset to 0 on successful frame
         * upload (in shm_to_texture / dmabuf path) so each new texture
         * gets a fresh chance. When it hits 5, the current texture is
         * marked invalid to stop the spam at its source.
         *
         * IMPORTANT: do NOT reset on every successful draw (else-branch).
         * Resetting per-draw would let a persistently-bad texture survive
         * past 5 errors just because of one lucky successful frame in
         * between. Reset only on NEW frame upload — that's the natural
         * boundary where a texture transition happens. */
        if (err != GL_NO_ERROR) {
            g_draw_err_count++;
            /* Throttle: log first 5 (capture onset), then every 300th (~5s
             * at 60fps) so we see persistent issues without flooding. */
            if (g_draw_err_count <= 5 || g_draw_err_count % 300 == 0) {
                fprintf(stderr,
                    "[idk-comp] GL error after draw: 0x%x (tex[%d]=%u fb=%dx%d frame=%ux%u, occurrence %d)\n",
                    err, g_tex_idx, g_tex[g_tex_idx], fb_w, fb_h,
                    g_frame_w, g_frame_h, g_draw_err_count);
            }
            /* If we keep failing on the same texture, it's probably dead —
             * mark it invalid AND delete it so the GL driver reclaims the
             * memory/ID. Without glDeleteTextures, the texture leaks: each
             * new frame creates a new texture (IDs climb 32, 33, 34, ...),
             * and at high FPS (glmark2-egl runs ~1800 FPS) this burns
             * through texture IDs fast and may exhaust driver resources. */
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
        /* No else-branch — g_draw_err_count is reset in shm_to_texture /
         * dmabuf path when a NEW frame is uploaded, not on every draw. */
    }
    glFinish();

    /* Check if display texture is valid */
    GLuint cur = g_tex[g_tex_idx];
    if (cur > 0 && !glIsTexture(cur)) {
        fprintf(stderr, "[idk-comp] WARNING: tex[%d] (%u) is not a valid texture!\n", g_tex_idx, cur);
        g_tex[g_tex_idx] = 0;
        g_has_frame = false;
    }

    /* ── Restore GL state (MangoHud-style) ── */
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
}

int idk_compositor_has_overlay(void) {
    return g_has_frame;
}

int idk_compositor_set_overlay(int dmabuf_fd, uint32_t w, uint32_t h,
                                uint32_t stride, uint32_t format) {
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
    unlink(g_sock_path);

    for (int i = 0; i < 2; i++) {
        if (g_tex[i]) glDeleteTextures(1, &g_tex[i]);
    }
    if (g_program) glDeleteProgram(g_program);

    g_has_frame = false;
    fprintf(stderr, "[idk-comp] Shut down\n");
}

/* ── GL resource initialization ────────────────────────────────────── */

/**
 * Initialize GL shaders and VBO. Call from GL context.
 */
int idk_compositor_init_gl(void) {
    /* Resolve all GL function pointers via dlsym (no link-time GL dep).
     * idk_gl_loader_init tries libGL.so.1, libGLESv2.so.2, etc. */
    if (idk_gl_loader_init() != 0) {
        fprintf(stderr, "[idk-comp] GL loader init failed — cannot init GL resources\n");
        return -1;
    }

    /* glEGLImageTargetTexture2DOES is an EGL extension, not in core GL.
     * Resolve it separately (it may be in libEGL.so or libGL.so). */
    void *libgl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!libgl) libgl = dlopen("libGL.so.1", RTLD_NOW);
    if (!libgl) libgl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_NOLOAD);
    if (!libgl) libgl = dlopen("libGLESv2.so.2", RTLD_NOW);
    if (!libgl) libgl = dlopen("libGL.so", RTLD_NOW);
    if (libgl) {
        fn_glEGLImageTargetTexture2DOES =
            (PFN_glEGLImageTargetTexture2DOES_fn)dlsym(libgl, "glEGLImageTargetTexture2DOES");
        fprintf(stderr, "[idk-comp] glEGLImageTargetTexture2DOES = %p\n",
                (void *)fn_glEGLImageTargetTexture2DOES);
    }

    /* ── Detect GL version (MangoHud GetOpenGLVersion) ───────────────── */
    {
        const char *version = (const char *)glGetString(GL_VERSION);
        if (version) {
            const char *es_prefixes[] = {
                "OpenGL ES-CM ",
                "OpenGL ES-CL ",
                "OpenGL ES ",
                NULL
            };
            g_is_gles = false;
            for (int i = 0; es_prefixes[i]; i++) {
                size_t plen = strlen(es_prefixes[i]);
                if (strncmp(version, es_prefixes[i], plen) == 0) {
                    version += plen;
                    g_is_gles = true;
                    break;
                }
            }
            int major = 0, minor = 0;
            sscanf(version, "%d.%d", &major, &minor);
            g_gl_version = major * 100 + minor * 10;
            if (g_is_gles && g_gl_version < 300)
                g_gl_version = 200;
            fprintf(stderr, "[idk-comp] GL version: %d.%d %s (g_gl_version=%d)\n",
                    major, minor, g_is_gles ? "ES" : "", g_gl_version);
        }
    }

    /* ── Select shader based on GLSL version (MangoHud-style) ────────── */
    int glsl_version = 120;
    if (!g_is_gles) {
        glsl_version = 120;
        if (g_gl_version >= 410)
            glsl_version = 410;
        else if (g_gl_version >= 320)
            glsl_version = 150;
        else if (g_gl_version >= 300)
            glsl_version = 130;
    } else {
        if (g_gl_version >= 300)
            glsl_version = 300;
        else
            glsl_version = 100;
    }

    const char *ver_str = NULL;
    const char *vs_body = NULL;
    const char *fs_body = NULL;

    if (glsl_version <= 120) {
        ver_str = g_is_gles ? "#version 100\n" : "#version 120\n";
        vs_body = overlay_vertex_120;
        fs_body = overlay_fragment_120;
    } else if (glsl_version == 300) {
        ver_str = "#version 300 es\n";
        vs_body = overlay_vertex_300_es;
        fs_body = overlay_fragment_300_es;
    } else if (glsl_version >= 410) {
        ver_str = "#version 410 core\n";
        vs_body = overlay_vertex_410;
        fs_body = overlay_fragment_410;
    } else {
        /* 130-409: use 130-style shaders */
        if (glsl_version >= 330)
            ver_str = "#version 330 core\n";
        else if (glsl_version >= 150)
            ver_str = "#version 150\n";
        else
            ver_str = "#version 130\n";
        vs_body = overlay_vertex_130;
        fs_body = overlay_fragment_130;
    }

    GLint vs_len = (GLint)(strlen(ver_str) + strlen(vs_body));
    GLint fs_len = (GLint)(strlen(ver_str) + strlen(fs_body));

    /* Build vertex shader source: version + body */
    char *vs_full = malloc((size_t)vs_len + 1);
    if (!vs_full) { fprintf(stderr, "[idk-comp] vs_full malloc failed\n"); return -1; }
    strcpy(vs_full, ver_str);
    strcat(vs_full, vs_body);

    /* Build fragment shader source: version + body */
    char *fs_full = malloc((size_t)fs_len + 1);
    if (!fs_full) { free(vs_full); fprintf(stderr, "[idk-comp] fs_full malloc failed\n"); return -1; }
    strcpy(fs_full, ver_str);
    strcat(fs_full, fs_body);

    fprintf(stderr, "[idk-comp] Using GLSL %s shader variant\n", ver_str);

    /* Compile vertex shader */
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    const char *vs_src = vs_full;
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);

    GLint ok;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar log[512];
        glGetShaderiv(vs, GL_INFO_LOG_LENGTH, &ok);
        if (ok > 0) {
            glGetShaderInfoLog(vs, 512, NULL, log);
            fprintf(stderr, "[idk-comp] VS log:\n%s\n", log);
        }
        glDeleteShader(vs);
        free(vs_full);
        free(fs_full);
        return -1;
    }

    /* Compile fragment shader */
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char *fs_src = fs_full;
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar log[512];
        glGetShaderiv(fs, GL_INFO_LOG_LENGTH, &ok);
        if (ok > 0) {
            glGetShaderInfoLog(fs, 512, NULL, log);
            fprintf(stderr, "[idk-comp] FS log:\n%s\n", log);
        }
        glDeleteShader(vs);
        glDeleteShader(fs);
        free(vs_full);
        free(fs_full);
        return -1;
    }

    free(vs_full);
    free(fs_full);

    /* Link program */
    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glLinkProgram(g_program);

    glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar log[512];
        glGetProgramiv(g_program, GL_INFO_LOG_LENGTH, &ok);
        if (ok > 0) {
            glGetProgramInfoLog(g_program, 512, NULL, log);
            fprintf(stderr, "[idk-comp] Link log: %s\n", log);
        }
        glDeleteProgram(g_program);
        g_program = 0;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return -1;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    fprintf(stderr, "[idk-comp] GL compositor ready (program=%u)\n",
            g_program);
    return 0;
}
