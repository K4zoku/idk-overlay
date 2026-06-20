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
#include "overlay_shader.h"
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
static GLuint g_vbo = 0;

/* Overlay texture — only one slot, newest frame */
static GLuint g_tex = 0;
static bool g_has_frame = false;

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
 * Convert SHM pixel data to GL texture via glTexImage2D.
 * Used as fallback when egl_dmabuf_to_texture fails (memfd is not a
 * real DMA-BUF, or EGL_EXT_image_dma_buf_import not available).
 *
 * Pixel format: RGBA8888 (matches Qt's QImage::Format_RGBA8888).
 */
static GLuint shm_to_texture(int shm_fd, uint32_t w, uint32_t h, uint32_t size) {
    if (!idk_fn_glTexImage2D) {
        fprintf(stderr, "[idk-comp] glTexImage2D not resolved\n");
        return 0;
    }

    /* mmap the SHM fd read-only */
    void *pixels = mmap(NULL, size, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "[idk-comp] SHM mmap failed: %s\n", strerror(errno));
        return 0;
    }

    /* Create GL texture and upload pixels */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    /* RGBA8888 unpacking — no row padding, 1 byte alignment */
    if (idk_fn_glPixelStorei) {
        idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }

    idk_fn_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                        (GLsizei)w, (GLsizei)h, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    munmap(pixels, size);
    return tex;
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
        /* SHM path: 'reserved' field = pixel byte size */
        uint32_t pixel_size = hdr.reserved;
        if (pixel_size == 0) pixel_size = hdr.width * hdr.height * 4;
        tex = shm_to_texture(dmabuf_fd, hdr.width, hdr.height, pixel_size);
        if (tex == 0) {
            fprintf(stderr, "[idk-comp] SHM texture upload failed\n");
            close(dmabuf_fd);
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
            tex = shm_to_texture(dmabuf_fd, hdr.width, hdr.height, pixel_size);
            if (tex == 0) {
                close(dmabuf_fd);
                return -1;
            }
        }
    }

    /* Clean up old texture, use new one */
    if (g_has_frame) {
        glDeleteTextures(1, &g_tex);
    }
    g_tex = tex;
    g_has_frame = true;
    g_frame_w = hdr.width;
    g_frame_h = hdr.height;

    fprintf(stderr, "[idk-comp] texture uploaded: tex=%u %ux%u pixel_size=%u\n",
            tex, hdr.width, hdr.height, hdr.reserved);

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
    if (!g_has_frame || g_program == 0) return;

    /* Get framebuffer dimensions */
    GLint fb_w, fb_h;
    {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        fb_w = vp[2];
        fb_h = vp[3];
    }
    if (fb_w <= 0 || fb_h <= 0) return;

    /* Calculate NDC coordinates (from -1 to 1) */
    float ndc_x = 2.0f * ((float)x / (float)fb_w) - 1.0f;
    float ndc_y = 1.0f - 2.0f * ((float)y / (float)fb_h);
    float ndc_w = 2.0f * ((float)w / (float)fb_w);
    float ndc_h = 2.0f * ((float)h / (float)fb_h);

    static int s_render_debug = 0;
    if (s_render_debug++ % 120 == 0) {
        fprintf(stderr, "[idk-comp] render_overlay: fb=%dx%d ndc=(%.1f,%.1f,%.1f,%.1f) tex=%u prog=%u\n",
                fb_w, fb_h, ndc_x, ndc_y, ndc_w, ndc_h, g_tex, g_program);
    }

    /* ── Save ALL GL state (adapted from imgoverlay/imgui_impl_opengl3) ── */
    GLenum last_active_texture; glGetIntegerv(GL_ACTIVE_TEXTURE, (GLint*)&last_active_texture);
    GLint last_program; glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
    GLint last_texture; glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    GLint last_array_buffer; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    GLint last_element_array_buffer; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_array_buffer);
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
    GLboolean last_enable_srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);

    /* Save vertex attrib array state for attribs 0-7 (MangoHud saves all) */
    GLint last_vaa[8];
    for (int i = 0; i < 8; i++) {
        glGetIntegerv(GL_VERTEX_ATTRIB_ARRAY_ENABLED + i, &last_vaa[i]);
    }

    /* ── Setup our render state (MangoHud-style) ── */
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    /* MangoHud uses GL_ONE for alpha src — preserves alpha channel,
     * prevents overlay from making game transparent on Wayland */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);

    /* Do NOT call glViewport — use the game's viewport.
     * We calculated NDC coords based on the game's viewport, so
     * changing it would break the overlay position. */

    glUseProgram(g_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glUniform1i(glGetUniformLocation(g_program, "u_texture"), 0);

    /* Vertex data: position (2 floats) + texCoord (2 floats) per vertex */
    float verts[] = {
        ndc_x,           ndc_y,           0.0f, 1.0f,
        ndc_x + ndc_w,   ndc_y,           1.0f, 1.0f,
        ndc_x + ndc_w,   ndc_y + ndc_h,   1.0f, 0.0f,
        ndc_x,           ndc_y,           0.0f, 1.0f,
        ndc_x + ndc_w,   ndc_y + ndc_h,   1.0f, 0.0f,
        ndc_x,           ndc_y + ndc_h,   0.0f, 0.0f,
    };

    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    GLint pos_loc = glGetAttribLocation(g_program, "a_position");
    GLint uv_loc  = glGetAttribLocation(g_program, "a_texCoord");
    if (pos_loc >= 0) {
        glEnableVertexAttribArray(pos_loc);
        glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    }
    if (uv_loc >= 0) {
        glEnableVertexAttribArray(uv_loc);
        glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);

    /* ── Restore ALL GL state (MangoHud-style) ── */
    if (pos_loc >= 0) glDisableVertexAttribArray(pos_loc);
    if (uv_loc >= 0) glDisableVertexAttribArray(uv_loc);
    for (int i = 0; i < 8; i++) {
        if (last_vaa[i]) glEnableVertexAttribArray(i);
    }

    glBlendEquationSeparate(last_blend_equation_rgb, last_blend_equation_alpha);
    glBlendFuncSeparate(last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha);
    if (last_enable_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (last_enable_cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (last_enable_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_enable_stencil_test) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (last_enable_scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (last_enable_srgb) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);

    /* Don't restore viewport — we never changed it */

    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_array_buffer);
    glBindTexture(GL_TEXTURE_2D, last_texture);
    glActiveTexture(last_active_texture);
    glUseProgram(last_program);
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

    if (g_tex) glDeleteTextures(1, &g_tex);
    if (g_vbo) glDeleteBuffers(1, &g_vbo);
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

    /* Compile vertex shader */
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    const char *vs_src = overlay_vertex_shader;
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);

    GLint ok;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar log[512];
        glGetShaderiv(vs, GL_INFO_LOG_LENGTH, &ok);
        if (ok > 0) {
            glGetShaderInfoLog(vs, 512, NULL, log);
            fprintf(stderr, "[idk-comp] VS log: %s\n", log);
        }
        glDeleteShader(vs);
        return -1;
    }

    /* Compile fragment shader */
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char *fs_src = overlay_fragment_shader;
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(fs);
        glDeleteShader(vs);
        return -1;
    }

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

    /* Create VBO for fullscreen quad (initially empty, updated each frame) */
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    fprintf(stderr, "[idk-comp] GL compositor ready (program=%u, vbo=%u)\n",
            g_program, g_vbo);
    return 0;
}
