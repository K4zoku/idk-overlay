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

#include "compositor.h"
#include "overlay_shader.h"
#include "idk_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Frame header from webview ─────────────────────────────────────── */

struct frame_hdr {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t num_planes;
    uint32_t reserved;
    uint32_t checksum;
};

/* ── EGL function pointers ─────────────────────────────────────────── */

typedef EGLImageKHR (*PFN_eglCreateImageKHR_fn)(EGLDisplay dpy, EGLContext ctx,
                                                 EGLenum target,
                                                 const EGLint *attrs);
typedef EGLBoolean (*PFN_eglDestroyImageKHR_fn)(EGLDisplay dpy, EGLImageKHR image);

static PFN_eglCreateImageKHR_fn fn_eglCreateImageKHR = NULL;
static PFN_eglDestroyImageKHR_fn fn_eglDestroyImageKHR = NULL;

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
int idk_compositor_render(void) {
    /* Try to accept client connection if none yet */
    accept_client();

    if (g_client_fd < 0) return -1;

    struct frame_hdr hdr;
    int dmabuf_fd = -1;

    /* Non-blocking poll for new frame */
    struct pollfd pfd = { .fd = g_client_fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
        return -1; /* no frame ready */
    }

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
    if (n < (ssize_t)sizeof(struct frame_hdr)) {
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

    if (dmabuf_fd < 0) return -1;

    memcpy(&hdr, msg_buf, sizeof(hdr));

    /* Upload to GL texture */
    GLuint tex = egl_dmabuf_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                       hdr.stride, hdr.format);
    if (tex == 0) {
        close(dmabuf_fd);
        return -1;
    }

    /* Clean up old texture, use new one */
    if (g_has_frame) {
        glDeleteTextures(1, &g_tex);
    }
    g_tex = tex;
    g_has_frame = true;
    g_frame_w = hdr.width;
    g_frame_h = hdr.height;

    /* Close the dmabuf fd — EGL texture keeps a reference */
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
        fprintf(stderr, "[idk-comp] EGL dma_buf import not available\n");
        return 0;
    }

    /* Get EGL display */
    EGLDisplay egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_dpy == EGL_NO_DISPLAY) {
        egl_dpy = eglGetDisplay((EGLNativeDisplayType)0);
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
        egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (const EGLint *)attrs);

    if (img == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "[idk-comp] eglCreateImageKHR failed: 0x%04X\n",
                (unsigned int)eglGetError());
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
 */
void idk_compositor_render_overlay(int x, int y, uint32_t w, uint32_t h) {
    if (!g_has_frame || g_program == 0) return;

    /* Get framebuffer dimensions for aspect ratio */
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    int fb_w = vp[2];
    int fb_h = vp[3];

    /* Calculate NDC coordinates (from -1 to 1) */
    float ndc_x = 2.0f * ((float)x / (float)fb_w) - 1.0f;
    float ndc_y = 1.0f - 2.0f * ((float)y / (float)fb_h);
    float ndc_w = 2.0f * ((float)w / (float)fb_w);
    float ndc_h = 2.0f * ((float)h / (float)fb_h);

    /* Save GL state */
    GLint old_blend_src, old_blend_dst;
    glGetIntegerv(GL_BLEND_SRC_RGB, &old_blend_src);
    glGetIntegerv(GL_BLEND_DST_RGB, &old_blend_dst);

    /* Enable blend for alpha transparency */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Bind texture and draw */
    glUseProgram(g_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glUniform1i(glGetUniformLocation(g_program, "u_overlay"), 0);

    /* Update VBO with quad coords */
    float verts[] = {
        ndc_x, ndc_y,              0.0f, 1.0f,  /* bottom-left */
        ndc_x + ndc_w, ndc_y,      1.0f, 1.0f,  /* bottom-right */
        ndc_x + ndc_w, ndc_y + ndc_h, 1.0f, 0.0f, /* top-right */
        ndc_x, ndc_y,              0.0f, 1.0f,  /* bottom-left (again) */
        ndc_x + ndc_w, ndc_y + ndc_h, 1.0f, 0.0f, /* top-right (again) */
        ndc_x, ndc_y + ndc_h,      0.0f, 0.0f,  /* top-left */
    };

    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Restore GL state */
    glBlendFunc((GLenum)old_blend_src, (GLenum)old_blend_dst);
    glDisable(GL_BLEND);
    glUseProgram(0);
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
    /* Resolve GL 2.0+ function pointers */
    void *libgl = dlopen("libGL.so", RTLD_NOW);
    if (!libgl) libgl = dlopen("libGL.so.1", RTLD_NOW);
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
