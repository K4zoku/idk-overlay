/* compositor.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
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
#include "core/compositor_common.h"
#include "public/idk_ipc.h"
#include "core/log.h"

/* Frame protocol definitions are in compositor_common.h (via idk_ipc.h).
 * idk_frame_header_t (28 bytes) replaces the old 40-byte struct frame_hdr.
 * Frame type is now a flag bit (IDK_FRAME_FLAG_DMABUF) instead of a
 * separate field, and premultiplied/visible are also flag bits. */

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
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT  0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT  0x3444
#define DRM_FORMAT_MOD_INVALID 0x00FFFFFFFFFFFFFFULL

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

/* ── GL_EXT_memory_object dmabuf import (GLX path, no EGL needed) ─────── */

/* GL handle types for EXT_external_memory */
#define GL_HANDLE_TYPE_DMA_BUF_EXT 0x330E

/* PFN types */
typedef unsigned long long GLu64;
typedef void (*PFN_glCreateMemoryObjectsEXT)(GLsizei, GLuint*);
typedef void (*PFN_glDeleteMemoryObjectsEXT)(GLsizei, const GLuint*);
typedef void (*PFN_glImportMemoryFdEXT)(GLuint, GLu64, GLenum, GLint);
typedef void (*PFN_glTexStorageMem2DEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLu64);

static PFN_glCreateMemoryObjectsEXT fn_glCreateMemoryObjectsEXT = NULL;
static PFN_glDeleteMemoryObjectsEXT fn_glDeleteMemoryObjectsEXT = NULL;
static PFN_glImportMemoryFdEXT      fn_glImportMemoryFdEXT      = NULL;
static PFN_glTexStorageMem2DEXT     fn_glTexStorageMem2DEXT     = NULL;
static int g_gl_mem_resolved = 0;
static int g_gl_mem_available = 0;

static void resolve_gl_memory_functions(void) {
    if (g_gl_mem_resolved) return;
    g_gl_mem_resolved = 1;

    /* Check extensions string */
    if (!idk_fn_glGetString) return;
    const GLubyte *exts = idk_fn_glGetString(0x1F03 /* GL_EXTENSIONS */);
    if (!exts) return;

    /* Need both GL_EXT_memory_object and GL_EXT_memory_object_fd */
    if (!strstr((const char *)exts, "GL_EXT_memory_object") ||
        !strstr((const char *)exts, "GL_EXT_memory_object_fd")) {
        IDK_LOG("comp", "GL_EXT_memory_object(_fd) not available\n");
        return;
    }

    /* Resolve via dlsym from libGL */
    void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libGL.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libGL.so", RTLD_NOW);
    if (!lib) return;

    fn_glCreateMemoryObjectsEXT = (PFN_glCreateMemoryObjectsEXT)dlsym(lib, "glCreateMemoryObjectsEXT");
    fn_glDeleteMemoryObjectsEXT = (PFN_glDeleteMemoryObjectsEXT)dlsym(lib, "glDeleteMemoryObjectsEXT");
    fn_glImportMemoryFdEXT      = (PFN_glImportMemoryFdEXT)dlsym(lib, "glImportMemoryFdEXT");
    fn_glTexStorageMem2DEXT     = (PFN_glTexStorageMem2DEXT)dlsym(lib, "glTexStorageMem2DEXT");

    if (fn_glCreateMemoryObjectsEXT && fn_glImportMemoryFdEXT && fn_glTexStorageMem2DEXT) {
        g_gl_mem_available = 1;
        IDK_LOG("comp", "GL_EXT_memory_object: available (CreateMem=%p ImportFd=%p TexStorageMem=%p)\n",
                (void*)fn_glCreateMemoryObjectsEXT, (void*)fn_glImportMemoryFdEXT,
                (void*)fn_glTexStorageMem2DEXT);
    } else {
        IDK_LOG("comp", "GL_EXT_memory_object: extension present but functions not resolved\n");
    }
}

/* Import dmabuf as GL texture via GL_EXT_memory_object (no EGL needed).
 * Returns texture ID on success, 0 on failure. Caller keeps fd ownership. */
static GLuint gl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h,
                                    uint32_t stride, uint32_t fourcc,
                                    uint64_t modifier) {
    resolve_gl_memory_functions();
    if (!g_gl_mem_available) return 0;

    /* Create memory object + import fd */
    GLuint mem = 0;
    fn_glCreateMemoryObjectsEXT(1, &mem);
    if (!mem) return 0;

    GLu64 size = (GLu64)stride * (GLu64)h;
    fn_glImportMemoryFdEXT(mem, size, GL_HANDLE_TYPE_DMA_BUF_EXT, dmabuf_fd);
    /* After import, the fd is owned by the GL driver — caller must NOT close it */

    /* Create texture backed by the memory object */
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /* GL_CLAMP_TO_EDGE */);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);

    /* GL_RGBA8 = 0x8058, internalformat for TexStorageMem2DEXT */
    fn_glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, 0x8058, w, h, mem, 0);

    glBindTexture(GL_TEXTURE_2D, 0);

    /* Memory object can be deleted after texture storage is allocated */
    fn_glDeleteMemoryObjectsEXT(1, &mem);

    IDK_LOG("comp", "GL dmabuf import OK: %ux%u tex=%u\n", w, h, tex);
    return tex;
}

static GLuint g_program = 0;


/* Double-buffered textures.
 * For DMABUF textures, we MUST keep the EGLImage and dmabuf_fd alive
 * for the texture's lifetime — Mesa's glEGLImageTargetTexStorageEXT
 * does NOT retain a reference to the dmabuf storage; if we destroy the
 * EGLImage or close the fd, the texture's backing is freed/reused
 * and renders as empty/white. */
static GLuint g_tex[2] = {0, 0};
static int    g_tex_w[2] = {0, 0};
static int    g_tex_h[2] = {0, 0};
static EGLImageKHR g_tex_img[2] = {0, 0};   /* EGLImage backing g_tex[i], or 0 */
static int    g_tex_dmabuf_fd[2] = {-1, -1}; /* fd backing g_tex[i], or -1 */
static int g_tex_idx = 0;   /* index of current display texture (0 or 1) */
static bool g_has_frame = false;
static bool g_frame_premultiplied = false;  /* current frame's alpha mode */

/* Free the DMABUF backing (EGLImage + fd) for slot i, if any.
 * Safe to call when slot i has a SHM texture (no-op). */
static void release_dmabuf_backing(int i) {
    if (i < 0 || i > 1) return;
    if (g_tex_img[i] && fn_eglDestroyImageKHR) {
        EGLDisplay dpy = fn_eglGetCurrentDisplay ? fn_eglGetCurrentDisplay() : EGL_NO_DISPLAY;
        if (dpy != EGL_NO_DISPLAY) {
            fn_eglDestroyImageKHR(dpy, g_tex_img[i]);
        }
    }
    g_tex_img[i] = 0;
    if (g_tex_dmabuf_fd[i] >= 0) {
        close(g_tex_dmabuf_fd[i]);
        g_tex_dmabuf_fd[i] = -1;
    }
}

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
static struct timespec g_last_resize_ts = {0};
static struct timespec g_last_frame_ts = {0};

void idk_compositor_notify_resize(int w, int h) {
    idk_comp_notify_resize(&g_game_w, &g_game_h, &g_size_pending,
                           &g_last_resize_ts, w, h, "comp");
}

typedef void* GLeglImage;
typedef void (*PFN_glEGLImageTargetTexture2DOES_fn)(GLenum target, GLeglImage image);
static PFN_glEGLImageTargetTexture2DOES_fn fn_glEGLImageTargetTexture2DOES = NULL;

/* GL_EXT_EGL_image_storage */
typedef void (*PFN_glEGLImageTargetTexStorageEXT_fn)(GLenum target, GLeglImage image, const GLint* attrib_list);
static PFN_glEGLImageTargetTexStorageEXT_fn fn_glEGLImageTargetTexStorageEXT = NULL;

/* Forward declaration — caller must keep *out_img alive for the
 * lifetime of the returned texture. */
static GLuint egl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h,
                                    uint32_t stride, uint32_t format,
                                    uint64_t modifier,
                                    EGLImageKHR *out_img);

static int g_listen_fd = -1;
static int g_client_fd = -1;
static char g_sock_path[512];

/* Init compositor — bind socket, accept client */
int idk_compositor_init(void) {
    if (idk_comp_sock_init(&g_listen_fd, g_sock_path, sizeof(g_sock_path), "comp") != 0) {
        return -1;
    }
    /* Try a non-blocking accept in case the webview is already waiting */
    idk_comp_sock_accept(g_listen_fd, &g_client_fd, "comp");
    g_has_frame = false;
    return 0;
}

/* Non-blocking accept */
static int accept_client(void) {
    return idk_comp_sock_accept(g_listen_fd, &g_client_fd, "comp");
}

/* SHM → GL texture upload via glTex(Sub)Image2D */
static GLuint shm_to_texture(int shm_fd, uint32_t w, uint32_t h,
                              uint32_t pixel_size, uint32_t buffer_idx,
                              int premultiplied) {
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

    /* If dimensions changed, delete old texture and reallocate via glTexImage2D.
     * Also force-delete if the slot was previously DMABUF-backed: a DMABUF
     * texture's storage is owned by the EGLImage and cannot be updated via
     * glTexSubImage2D — we must recreate it as a regular GL texture. */
    bool size_changed = (GLsizei)w != g_tex_w[back] || (GLsizei)h != g_tex_h[back];
    bool was_dmabuf = (g_tex_img[back] != 0) || (g_tex_dmabuf_fd[back] >= 0);
    if ((size_changed || was_dmabuf) && g_tex[back] != 0) {
        glDeleteTextures(1, &g_tex[back]);
        g_tex[back] = 0;
    }
    /* Replacing a slot that was previously DMABUF-backed: free the backing. */
    if (was_dmabuf) {
        release_dmabuf_backing(back);
    }

    if (g_tex[back] == 0) {
        glGenTextures(1, &g_tex[back]);
        glBindTexture(GL_TEXTURE_2D, g_tex[back]);
        if (idk_fn_glPixelStorei) {
            idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        }
        /* Use GL_RGBA8 for straight, GL_RGBA8 for premultiplied too —
         * the difference is in the blend mode at render time, not the
         * internal format. The data layout is identical (4 bytes RGBA).
         * Premultiplied flag is stored for the render pass to set
         * the correct blend equation. */
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
    g_frame_premultiplied = (premultiplied != 0);
    g_draw_err_count = 0;  /* fresh texture — reset draw error counter */

    IDK_LOG("comp", "SHM frame uploaded: %ux%u tex[%d]=%u premul=%d buf_idx=%u\n",
            (unsigned)w, (unsigned)h, back, g_tex[back],
            (int)g_frame_premultiplied, (unsigned)buffer_idx);

    /* Don't close shm_fd if we stored it as g_shm_fd — the mmap relies on
     * the fd staying open. If it's a different fd (not stored), close it. */
    if (shm_fd != g_shm_fd) {
        close(shm_fd);
    }

    return g_tex[g_tex_idx];
}

int idk_compositor_render(void) {
    accept_client();
    if (g_client_fd < 0) return -1;

    /* Poll for new frame — keep newest only */
    int processed = 0;

    while (1) {
        idk_frame_header_t hdr;
        int dmabuf_fd = -1;
        int rc = idk_comp_recv_frame(g_client_fd, &hdr, &dmabuf_fd, "comp");
        if (rc <= 0) {
            if (rc < 0) {
                fprintf(stderr,
                    "[idk-comp] client disconnected (fd=%d), resetting\n",
                    g_client_fd);
                close(g_client_fd);
                g_client_fd = -1;
            }
            break;
        }

        if (processed > 0) {
            close(dmabuf_fd);
            continue;
        }

        GLuint tex = 0;

        if (!idk_frame_is_dmabuf(&hdr)) {
            /* SHM frame — compute pixel size from dimensions (4 bytes/pixel) */
            uint32_t pixel_size = hdr.width * hdr.height * 4;
            /* buf_idx is no longer in the header; use 0 (single buffer).
             * Premultiplied flag is not in the new header — assume
             * premultiplied for SHM (Qt RHI always premultiplies). */
            tex = shm_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                 pixel_size, 0, 1);
            if (tex == 0) break;
            g_frame_w = hdr.width;
            g_frame_h = hdr.height;
            processed = 1;
            clock_gettime(CLOCK_MONOTONIC, &g_last_frame_ts);
        } else {
            /* DMABUF frame — try EGL first, then GL_EXT_memory_object (GLX).
             * Under GLX (e.g. glxgears), there's no EGL display, so
             * eglCreateImageKHR can't work. Fall back to GL_EXT_memory_object
             * which works with any GL context (GLX or EGL). */
            EGLDisplay check_dpy = EGL_NO_DISPLAY;
            if (fn_eglGetCurrentDisplay) {
                check_dpy = fn_eglGetCurrentDisplay();
            }

            GLuint tex = 0;
            EGLImageKHR img = 0;
            int fd_consumed = 0;  /* set if GL driver took fd ownership */

            if (check_dpy != EGL_NO_DISPLAY) {
                /* EGL path — import via eglCreateImageKHR */
                tex = egl_dmabuf_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                             hdr.stride, hdr.fourcc,
                                             hdr.modifier, &img);
            }

            if (tex == 0) {
                /* EGL failed or unavailable — try GL_EXT_memory_object */
                tex = gl_dmabuf_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                            hdr.stride, hdr.fourcc,
                                            hdr.modifier);
                if (tex) {
                    /* GL driver consumed the fd — don't close it */
                    fd_consumed = 1;
                }
            }

            if (tex == 0) {
                /* Both EGL and GL_EXT_memory_object failed.
                 * If EGL was unavailable entirely (GLX-only), reject DMABUF
                 * so webview falls back to SHM. If EGL was available but
                 * import failed (transient), drop frame and retry. */
                if (check_dpy == EGL_NO_DISPLAY && !g_gl_mem_available) {
                    /* No EGL + no GL_EXT_memory_object — can't import DMABUF at all */
                    IDK_LOG("comp", "no EGL display + no GL_EXT_memory_object — rejecting DMABUF, forcing SHM\n");
                    close(dmabuf_fd);
                    processed = -1;  /* ACK=1 → webview falls back to SHM */
                } else {
                    /* Transient failure — drop frame, ACK=0 to retry */
                    IDK_LOG("comp", "DMABUF import failed (transient) — dropping frame\n");
                    if (img && fn_eglDestroyImageKHR) {
                        EGLDisplay dpy = fn_eglGetCurrentDisplay ? fn_eglGetCurrentDisplay() : EGL_NO_DISPLAY;
                        if (dpy != EGL_NO_DISPLAY) fn_eglDestroyImageKHR(dpy, img);
                    }
                    close(dmabuf_fd);
                    processed = 1;
                }
            } else {
                int back = 1 - g_tex_idx;
                /* Replacing previous slot: delete its texture AND any
                 * DMABUF backing (EGLImage + fd) so we don't leak. */
                if (g_tex[back]) glDeleteTextures(1, &g_tex[back]);
                release_dmabuf_backing(back);
                /* Install new texture + keep its backing alive. */
                g_tex[back] = tex;
                g_tex_img[back] = img;
                g_tex_dmabuf_fd[back] = fd_consumed ? -1 : dmabuf_fd;  /* -1 if GL consumed fd */
                g_tex_w[back] = (GLsizei)hdr.width;
                g_tex_h[back] = (GLsizei)hdr.height;
                g_tex_idx = back;
                g_has_frame = true;
                /* DMABUF from Qt RHI is always premultiplied alpha */
                g_frame_premultiplied = true;
                g_draw_err_count = 0;
                g_frame_w = hdr.width;
                g_frame_h = hdr.height;
                processed = 1;
                clock_gettime(CLOCK_MONOTONIC, &g_last_frame_ts);
            }
        }
    }

    if (processed) {
        uint8_t ack = (processed < 0) ? 1 : 0;
        /* GL path: no debounce (debounce_ms=0 sends size immediately).
         * VK path uses IDK_COMP_RESIZE_DEBOUNCE_MS. */
        idk_comp_send_ack(g_client_fd, ack,
                          g_game_w, g_game_h,
                          &g_size_pending, &g_last_resize_ts,
                          0, "comp");
        return 0;
    }
    return -1;
}

GLuint egl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h,
                              uint32_t stride, uint32_t format,
                              uint64_t modifier,
                              EGLImageKHR *out_img) {
    if (out_img) *out_img = 0;
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

    /* Build attribute list. Include modifier if it's valid (not
     * DRM_FORMAT_MOD_INVALID). Without the modifier, Mesa assumes
     * linear layout — if the dmabuf is actually tiled/compressed,
     * the imported texture will have garbled/white content. */
    EGLint attrs[16];
    int ai = 0;
    attrs[ai++] = EGL_WIDTH;
    attrs[ai++] = (EGLint)w;
    attrs[ai++] = EGL_HEIGHT;
    attrs[ai++] = (EGLint)h;
    attrs[ai++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[ai++] = (EGLint)drm_fmt;
    attrs[ai++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attrs[ai++] = dmabuf_fd;
    attrs[ai++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attrs[ai++] = 0;
    attrs[ai++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attrs[ai++] = (EGLint)stride;
    if (modifier != 0 && modifier != DRM_FORMAT_MOD_INVALID) {
        attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attrs[ai++] = (EGLint)(modifier & 0xFFFFFFFF);
        attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attrs[ai++] = (EGLint)((modifier >> 32) & 0xFFFFFFFF);
    }
    attrs[ai++] = EGL_NONE;

    IDK_LOG("comp", "eglCreateImage: %ux%u fourcc=0x%x stride=%u modifier=0x%llx\n",
            (unsigned)w, (unsigned)h, (unsigned)drm_fmt, (unsigned)stride,
            (unsigned long long)modifier);

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

    /* Resolve both EGL image binding functions up front */
    if (!fn_glEGLImageTargetTexture2DOES) {
        void *lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libOpenGL.so.0", RTLD_NOW);
        if (lib) {
            fn_glEGLImageTargetTexture2DOES =
                (PFN_glEGLImageTargetTexture2DOES_fn)dlsym(lib, "glEGLImageTargetTexture2DOES");
        }
    }
    if (!fn_glEGLImageTargetTexture2DOES && fn_eglGetProcAddress) {
        fn_glEGLImageTargetTexture2DOES = (PFN_glEGLImageTargetTexture2DOES_fn)
            fn_eglGetProcAddress("glEGLImageTargetTexture2DOES");
    }
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

    IDK_LOG("comp", "EGL image bind funcs: TexStorage=%p Texture2DOES=%p (w=%u h=%u stride=%u fourcc=0x%x)\n",
            (void*)fn_glEGLImageTargetTexStorageEXT,
            (void*)fn_glEGLImageTargetTexture2DOES,
            (unsigned)w, (unsigned)h, (unsigned)stride, (unsigned)drm_fmt);

    /* Try glEGLImageTargetTexStorageEXT FIRST — correct for desktop GL.
     * It allocates texture storage from the EGLImage.
     * glEGLImageTargetTexture2DOES is GLES-only; on desktop GL it may
     * silently "succeed" (GL_NO_ERROR) but leave texture unallocated (= white). */
    GLboolean ok = GL_FALSE;
    GLenum err = GL_NO_ERROR;
    static int s_texstorage_success_logged = 0;

    if (fn_glEGLImageTargetTexStorageEXT) {
        fn_glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, (GLeglImage)img, NULL);
        err = glGetError();
        if (err == GL_NO_ERROR) {
            ok = GL_TRUE;
            /* Log success only once to avoid per-frame spam */
            if (!s_texstorage_success_logged) {
                IDK_LOG("comp", "EGL image bound via TexStorageEXT (will not re-log)\n");
                s_texstorage_success_logged = 1;
            }
        } else {
            IDK_LOG("comp", "TexStorageEXT failed (0x%04X), trying Texture2DOES\n", err);
        }
    }

    if (!ok && fn_glEGLImageTargetTexture2DOES) {
        while (glGetError() != GL_NO_ERROR) {}
        fn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImage)img);
        err = glGetError();
        if (err == GL_NO_ERROR) {
            ok = GL_TRUE;
            IDK_LOG("comp", "EGL image bound via Texture2DOES\n");
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

    /* DO NOT destroy the EGLImage here — Mesa's TexStorageEXT does NOT
     * retain a reference to the dmabuf storage; if we destroy the image
     * (or the caller closes the dmabuf fd), the texture's backing memory
     * gets freed/reused and the texture renders as empty/white.
     * The caller is responsible for keeping *out_img (and the dmabuf fd)
     * alive for the texture's lifetime, and destroying them when the
     * texture is replaced/deleted. */
    if (out_img) *out_img = img;

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
    /* Blend mode depends on frame format:
     * - Straight alpha: src=GL_SRC_ALPHA, dst=GL_ONE_MINUS_SRC_ALPHA
     * - Premultiplied:  src=GL_ONE, dst=GL_ONE_MINUS_SRC_ALPHA (faster, no multiply in shader) */
    if (g_frame_premultiplied) {
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
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
                release_dmabuf_backing(g_tex_idx);
                g_has_frame = false;
            }
        }
        /* Reset only on new frame upload in shm_to_texture / dmabuf path */
    }
    GLuint cur = g_tex[g_tex_idx];
    if (cur > 0 && !glIsTexture(cur)) {
        IDK_ERR("comp", "WARNING: tex[%d] (%u) is not a valid texture!\n", g_tex_idx, cur);
        g_tex[g_tex_idx] = 0;
        release_dmabuf_backing(g_tex_idx);
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
    /* Release DMABUF backing (EGLImage + fd) for both slots */
    release_dmabuf_backing(0);
    release_dmabuf_backing(1);
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
