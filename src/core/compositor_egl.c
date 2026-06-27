/* compositor_egl.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#include "gl/gl_loader.h"        /* GL types + function pointer redirects */
#include "gl/shader_loader.h"    /* shader compile + SPIR-V fallback */
#include "core/compositor_egl.h"
#include "core/compositor_common.h"
#include "core/transport.h"
#include "public/idk_ipc.h"
#include "core/log.h"

/* Frame protocol via idk_ipc.h (28B header, flag bits for type/alpha). */

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

/* EGL constants for hidden context creation */
#define EGL_OPENGL_API          0x30A2
#define EGL_OPENGL_BIT          0x0008
#define EGL_PBUFFER_BIT         0x0001
#define EGL_SURFACE_TYPE        0x3033
#define EGL_RENDERABLE_TYPE     0x3040
#define EGL_RED_SIZE            0x3024
#define EGL_GREEN_SIZE          0x3023
#define EGL_BLUE_SIZE           0x3022
#define EGL_ALPHA_SIZE          0x3021
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_LUMINANCE_SIZE      0x303D
#define EGL_COLOR_BUFFER_TYPE   0x303F
#define EGL_RGB_BUFFER          0x308E

typedef EGLDisplay (*PFN_eglGetDisplay_fn)(EGLNativeDisplayType);
typedef EGLDisplay (*PFN_eglGetCurrentDisplay_fn)(void);
typedef EGLint (*PFN_eglGetError_fn)(void);
typedef void* (*PFN_eglGetProcAddress_fn)(const char*);
typedef EGLImageKHR (*PFN_eglCreateImageKHR_fn)(EGLDisplay dpy, EGLContext ctx,
                                                 EGLenum target,
                                                 void *buffer,
                                                 const EGLint *attrs);
typedef EGLBoolean (*PFN_eglDestroyImageKHR_fn)(EGLDisplay dpy, EGLImageKHR image);
typedef EGLBoolean (*PFN_eglInitialize_fn)(EGLDisplay dpy, EGLint *major, EGLint *minor);
typedef EGLBoolean (*PFN_eglChooseConfig_fn)(EGLDisplay dpy, const EGLint *attribs,
                                              EGLSurface *configs, EGLint config_size,
                                              EGLint *num_config);
typedef EGLSurface (*PFN_eglCreatePbufferSurface_fn)(EGLDisplay dpy, EGLSurface config,
                                                      const EGLint *attribs);
typedef EGLContext (*PFN_eglCreateContext_fn)(EGLDisplay dpy, EGLSurface config,
                                               EGLContext share, const EGLint *attribs);
typedef EGLBoolean (*PFN_eglMakeCurrent_fn)(EGLDisplay dpy, EGLSurface draw,
                                             EGLSurface read, EGLContext ctx);
typedef EGLBoolean (*PFN_eglDestroyContext_fn)(EGLDisplay dpy, EGLContext ctx);
typedef EGLBoolean (*PFN_eglDestroySurface_fn)(EGLDisplay dpy, EGLSurface surf);
typedef EGLBoolean (*PFN_eglBindAPI_fn)(EGLenum api);


static PFN_eglGetDisplay_fn          fn_eglGetDisplay          = NULL;
static PFN_eglGetCurrentDisplay_fn   fn_eglGetCurrentDisplay   = NULL;
static PFN_eglGetError_fn            fn_eglGetError            = NULL;
static PFN_eglGetProcAddress_fn      fn_eglGetProcAddress      = NULL;
static PFN_eglCreateImageKHR_fn      fn_eglCreateImageKHR      = NULL;
static PFN_eglDestroyImageKHR_fn     fn_eglDestroyImageKHR     = NULL;
static PFN_eglInitialize_fn          fn_eglInitialize          = NULL;
static PFN_eglChooseConfig_fn        fn_eglChooseConfig        = NULL;
static PFN_eglCreatePbufferSurface_fn fn_eglCreatePbufferSurface = NULL;
static PFN_eglCreateContext_fn       fn_eglCreateContext       = NULL;
static PFN_eglMakeCurrent_fn         fn_eglMakeCurrent         = NULL;
static PFN_eglDestroyContext_fn      fn_eglDestroyContext      = NULL;
static PFN_eglDestroySurface_fn      fn_eglDestroySurface      = NULL;
static PFN_eglBindAPI_fn             fn_eglBindAPI             = NULL;

/* Hidden compositor EGL context state — UNUSED after GLX→SHM fallback fix.
 * See ensure_compositor_egl_context() (also #if 0'd) for context. */
#if 0
static EGLDisplay g_compositor_dpy   = NULL;
static EGLSurface g_compositor_surf  = NULL;
static EGLContext g_compositor_ctx   = NULL;
static int g_compositor_egl_inited   = 0;
#endif

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
    fn_eglInitialize        = (PFN_eglInitialize_fn)         dlsym(lib, "eglInitialize");
    fn_eglChooseConfig      = (PFN_eglChooseConfig_fn)       dlsym(lib, "eglChooseConfig");
    fn_eglCreatePbufferSurface = (PFN_eglCreatePbufferSurface_fn) dlsym(lib, "eglCreatePbufferSurface");
    fn_eglCreateContext     = (PFN_eglCreateContext_fn)      dlsym(lib, "eglCreateContext");
    fn_eglMakeCurrent       = (PFN_eglMakeCurrent_fn)        dlsym(lib, "eglMakeCurrent");
    fn_eglDestroyContext    = (PFN_eglDestroyContext_fn)     dlsym(lib, "eglDestroyContext");
    fn_eglDestroySurface    = (PFN_eglDestroySurface_fn)     dlsym(lib, "eglDestroySurface");
    fn_eglBindAPI           = (PFN_eglBindAPI_fn)            dlsym(lib, "eglBindAPI");

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

/* Hidden compositor-side EGL context — UNUSED after GLX→SHM fallback fix.
 *
 * Previously created to enable DMABUF import for GLX apps (which have no
 * current EGL display). But Mesa's glEGLImageTargetTexStorageEXT crashes
 * when binding an EGLImage from one EGLDisplay to a texture in a GLX-backed
 * GL context (the EGLImage's display must match the current context's
 * display). For GLX apps, we now reject DMABUF and use the SHM path.
 *
 * Kept here for reference — re-enable if Mesa ever adds cross-display
 * EGLImage sharing, or if we implement a CPU-copy fallback path.
 */
#if 0
static EGLDisplay ensure_compositor_egl_context(void) {
    if (g_compositor_egl_inited) return g_compositor_dpy;
    g_compositor_egl_inited = 1;

    if (!fn_eglGetDisplay) resolve_egl_functions();
    if (!fn_eglGetDisplay || !fn_eglInitialize || !fn_eglChooseConfig ||
        !fn_eglCreateContext || !fn_eglCreatePbufferSurface ||
        !fn_eglBindAPI || !fn_eglMakeCurrent) {
        IDK_ERR("comp", "EGL context functions not resolved\n");
        return NULL;
    }

    g_compositor_dpy = fn_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!g_compositor_dpy) {
        IDK_ERR("comp", "eglGetDisplay(default) failed: 0x%x\n",
                fn_eglGetError ? fn_eglGetError() : 0);
        return NULL;
    }

    EGLint major = 0, minor = 0;
    if (!fn_eglInitialize(g_compositor_dpy, &major, &minor)) {
        IDK_ERR("comp", "eglInitialize failed: 0x%x\n",
                fn_eglGetError ? fn_eglGetError() : 0);
        g_compositor_dpy = NULL;
        return NULL;
    }

    if (!fn_eglBindAPI(EGL_OPENGL_API)) {
        IDK_ERR("comp", "eglBindAPI(OpenGL) failed: 0x%x\n",
                fn_eglGetError ? fn_eglGetError() : 0);
        return NULL;
    }

    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };
    EGLSurface configs[8];
    EGLint num_config = 0;
    if (!fn_eglChooseConfig(g_compositor_dpy, cfg_attrs, configs, 8, &num_config) ||
        num_config == 0) {
        IDK_ERR("comp", "eglChooseConfig failed: 0x%x (num=%d)\n",
                fn_eglGetError ? fn_eglGetError() : 0, num_config);
        return NULL;
    }
    EGLSurface cfg = configs[0];

    EGLint pbuf_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    g_compositor_surf = fn_eglCreatePbufferSurface(g_compositor_dpy, cfg, pbuf_attrs);
    if (!g_compositor_surf) {
        IDK_ERR("comp", "eglCreatePbufferSurface failed: 0x%x\n",
                fn_eglGetError ? fn_eglGetError() : 0);
        return NULL;
    }

    EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE,
    };
    g_compositor_ctx = fn_eglCreateContext(g_compositor_dpy, cfg, NULL, ctx_attrs);
    if (!g_compositor_ctx) {
        /* Retry with no version attrs (let driver pick default) */
        g_compositor_ctx = fn_eglCreateContext(g_compositor_dpy, cfg, NULL, NULL);
    }
    if (!g_compositor_ctx) {
        IDK_ERR("comp", "eglCreateContext failed: 0x%x\n",
                fn_eglGetError ? fn_eglGetError() : 0);
        return NULL;
    }

    IDK_LOG("comp", "compositor EGL context ready (dpy=%p surf=%p ctx=%p, EGL %d.%d)\n",
            (void*)g_compositor_dpy, (void*)g_compositor_surf, (void*)g_compositor_ctx,
            major, minor);
    return g_compositor_dpy;
}
#endif

/* ── GL_EXT_memory_object dmabuf import (MangoHud approach) ────────────
 *
 * Per GL_EXT_memory_object_fd spec, only GL_HANDLE_TYPE_OPAQUE_FD_EXT is
 * defined — there is NO GL_HANDLE_TYPE_DMA_BUF_EXT. Spec says OPAQUE_FD is
 * for fds created via glGetMemoryObjectFdEXT (GL→GL roundtrip).
 *
 * MangoHud's trick: pass dmabuf fds to glImportMemoryFdEXT with
 * GL_HANDLE_TYPE_OPAQUE_FD_EXT. Mesa's driver accepts this and handles
 * the dmabuf correctly based on the actual underlying fd type. This is
 * technically not spec-compliant but works on all Mesa drivers.
 *
 * This is the GLX path (no EGL needed) — works on any GL context.
 * EGL apps can still use the EGL_LINUX_DMA_BUF_EXT path above.
 */

#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x22C1
#define GL_RGBA8                     0x8058
#define GL_TEXTURE_SWIZZLE_R         0x8E42
#define GL_TEXTURE_SWIZZLE_G         0x8E43
#define GL_TEXTURE_SWIZZLE_B         0x8E44
#define GL_TEXTURE_SWIZZLE_A         0x8E45
#define GL_ALPHA_ENUM                0x1906
#define GL_BLUE                      0x1905
#define GL_GREEN                     0x1904
#define GL_RED                       0x1903

typedef unsigned long long GLu64;
typedef void (*PFN_glCreateMemoryObjectsEXT_fn)(GLsizei, GLuint*);
typedef void (*PFN_glDeleteMemoryObjectsEXT_fn)(GLsizei, const GLuint*);
typedef void (*PFN_glImportMemoryFdEXT_fn)(GLuint, GLu64, GLenum, GLint);
typedef void (*PFN_glTexStorageMem2DEXT_fn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLu64);

static PFN_glCreateMemoryObjectsEXT_fn fn_glCreateMemoryObjectsEXT = NULL;
static PFN_glDeleteMemoryObjectsEXT_fn fn_glDeleteMemoryObjectsEXT = NULL;
static PFN_glImportMemoryFdEXT_fn      fn_glImportMemoryFdEXT      = NULL;
static PFN_glTexStorageMem2DEXT_fn     fn_glTexStorageMem2DEXT     = NULL;
static int g_gl_mem_resolved = 0;
static int g_gl_mem_available = 0;

static void resolve_gl_memory_functions(void) {
    if (g_gl_mem_resolved) return;
    g_gl_mem_resolved = 1;

    if (!idk_fn_glGetString) return;
    const GLubyte *exts = idk_fn_glGetString(0x1F03 /* GL_EXTENSIONS */);
    if (!exts) return;

    if (!strstr((const char *)exts, "GL_EXT_memory_object") ||
        !strstr((const char *)exts, "GL_EXT_memory_object_fd")) {
        IDK_LOG("comp", "GL_EXT_memory_object(_fd) not available\n");
        return;
    }

    void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libGL.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libGL.so", RTLD_NOW);
    if (!lib) return;

    fn_glCreateMemoryObjectsEXT = (PFN_glCreateMemoryObjectsEXT_fn)dlsym(lib, "glCreateMemoryObjectsEXT");
    fn_glDeleteMemoryObjectsEXT = (PFN_glDeleteMemoryObjectsEXT_fn)dlsym(lib, "glDeleteMemoryObjectsEXT");
    fn_glImportMemoryFdEXT      = (PFN_glImportMemoryFdEXT_fn)dlsym(lib, "glImportMemoryFdEXT");
    fn_glTexStorageMem2DEXT     = (PFN_glTexStorageMem2DEXT_fn)dlsym(lib, "glTexStorageMem2DEXT");

    if (fn_glCreateMemoryObjectsEXT && fn_glImportMemoryFdEXT && fn_glTexStorageMem2DEXT) {
        g_gl_mem_available = 1;
        IDK_LOG("comp", "GL_EXT_memory_object: available (MangoHud-style dmabuf import)\n");
    } else {
        IDK_LOG("comp", "GL_EXT_memory_object: extension present but functions not resolved\n");
    }
}

/* Import dmabuf as GL texture via GL_EXT_memory_object (MangoHud approach).
 * Returns texture ID on success, 0 on failure. Caller keeps fd ownership.
 *
 * The GL driver takes ownership of the dup'd fd — caller's original fd
 * must be kept alive separately (we track it in g_tex_dmabuf_fd[] for
 * cache invalidation on resize). */
static GLuint gl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h,
                                    uint32_t stride, uint32_t fourcc) {
    resolve_gl_memory_functions();
    if (!g_gl_mem_available) return 0;

    /* Drain any stale GL errors so we can detect new ones cleanly.
     * Multiple iterations because some drivers queue multiple errors. */
    if (idk_fn_glGetError) {
        GLenum e;
        int drains = 0;
        while ((e = idk_fn_glGetError()) != GL_NO_ERROR && drains < 10) drains++;
        if (drains > 0) {
            static int s_drain_logged = 0;
            if (!s_drain_logged) {
                IDK_LOG("comp", "gl_dmabuf_to_texture: drained %d stale GL errors\n", drains);
                s_drain_logged = 1;
            }
        }
    }

    /* Create memory object */
    GLuint mem = 0;
    fn_glCreateMemoryObjectsEXT(1, &mem);
    /* Drain any error from CreateMemoryObjectsEXT */
    if (idk_fn_glGetError) { while (idk_fn_glGetError() != GL_NO_ERROR); }
    if (!mem) {
        IDK_ERR("comp", "glCreateMemoryObjectsEXT returned 0\n");
        return 0;
    }

    /* dup() the fd — glImportMemoryFdEXT takes ownership of the passed fd */
    int import_fd = dup(dmabuf_fd);
    if (import_fd < 0) {
        IDK_ERR("comp", "dup(dmabuf_fd=%d) failed: %s\n", dmabuf_fd, strerror(errno));
        fn_glDeleteMemoryObjectsEXT(1, &mem);
        return 0;
    }

    /* MangoHud trick: use GL_HANDLE_TYPE_OPAQUE_FD_EXT for dmabuf fds.
     * Mesa's driver detects the actual fd type and handles dmabuf correctly.
     * The spec only defines OPAQUE_FD for GL-exported fds, but Mesa extends
     * this to accept dmabuf fds as well. */
    GLu64 size = (GLu64)stride * (GLu64)h;
    fn_glImportMemoryFdEXT(mem, size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, import_fd);
    /* After ImportMemoryFdEXT (success), import_fd is owned by GL driver.
     * On failure, we must close it ourselves. */

    GLenum err = idk_fn_glGetError ? idk_fn_glGetError() : GL_NO_ERROR;
    if (err != GL_NO_ERROR) {
        IDK_ERR("comp", "glImportMemoryFdEXT failed: 0x%04x (fd=%d size=%llu stride=%u h=%u)\n",
                err, import_fd, (unsigned long long)size, stride, (unsigned int)h);
        close(import_fd);
        fn_glDeleteMemoryObjectsEXT(1, &mem);
        return 0;
    }

    /* Create texture backed by the memory object */
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    /* GL_RGBA8 matches 4-bytes-per-pixel BGRA/RGBA/ABGR dmabuf formats
     * when combined with the swizzle below. */
    fn_glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, w, h, mem, 0);

    err = idk_fn_glGetError ? idk_fn_glGetError() : GL_NO_ERROR;
    if (err != GL_NO_ERROR) {
        IDK_ERR("comp", "glTexStorageMem2DEXT failed: 0x%04x (w=%u h=%u)\n", err, w, h);
        glDeleteTextures(1, &tex);
        fn_glDeleteMemoryObjectsEXT(1, &mem);
        return 0;
    }

    /* Swizzle for ABGR8888 / BGRA8888 dmabuf formats.
     * Both have R and B channels swapped vs GL_RGBA8's expected layout.
     * Swizzle R↔B to get correct color output.
     * (MangoHud does the same.)
     *
     * If fourcc indicates RGBA8888 (0x34324752), skip swizzle.
     * For everything else (AB24/AR24/BG24/ etc.), apply R↔B swap. */
    if (fourcc != 0x34324752 /* DRM_FORMAT_RGBA8888 */) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA_ENUM);
    }

    /* Set basic filtering/wrap — caller will override if needed */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /* GL_CLAMP_TO_EDGE */);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);

    glBindTexture(GL_TEXTURE_2D, 0);

    /* Memory object can be deleted after texture storage is allocated.
     * The texture retains its own reference to the imported memory. */
    fn_glDeleteMemoryObjectsEXT(1, &mem);

    IDK_LOG("comp", "GL dmabuf import OK (MangoHud-style): %ux%u tex=%u fourcc=0x%x\n",
            w, h, tex, fourcc);
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

void idk_compositor_egl_notify_resize(int w, int h) {
    bool changed = idk_comp_notify_resize(&g_game_w, &g_game_h, &g_size_pending,
                                          &g_last_resize_ts, w, h, "comp");
    if (changed) {
        /* Game viewport changed — invalidate ALL cached DMABUF textures.
         * Stale EGLImage/fd from pre-resize frames can render as black
         * or garbage if Qt RHI rebuilds its render-target texture with
         * new dimensions/modifier. Forcing fresh import on next frame. */
        for (int i = 0; i < 2; i++) {
            if (g_tex[i]) {
                glDeleteTextures(1, &g_tex[i]);
                g_tex[i] = 0;
            }
            release_dmabuf_backing(i);
            g_tex_w[i] = 0;
            g_tex_h[i] = 0;
        }
        g_has_frame = false;
        g_tex_idx = 0;
        IDK_LOG("comp", "resize: invalidated DMABUF texture cache\n");
    }
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

static char g_sock_path[512];
static idk_transport_t g_tp;

/* Init compositor — init transport, accept client */
int idk_compositor_egl_init(void) {
    static int g_inited = 0;
    if (g_inited) return 0;
    g_inited = 1;

    idk_comp_get_path(g_sock_path, sizeof(g_sock_path));
    if (idk_tp_init(&g_tp, IDK_TP_CONSUMER, g_sock_path) != 0) {
        return -1;
    }
    /* Try a non-blocking accept in case the webview is already waiting */
    idk_tp_accept(&g_tp);
    g_has_frame = false;
    return 0;
}

/* Non-blocking accept */
static int accept_client(void) {
    return idk_tp_accept(&g_tp);
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
        IDK_ERR("comp", "shm_to_texture: rejecting zero-dim frame w=%u h=%u\n", w, h);
        return 0;
    }
    uint32_t expected = w * h * 4;
    if (pixel_size < expected) {
        IDK_ERR("comp", "shm_to_texture: size mismatch w=%u h=%u pixel_size=%u expected=%u\n",
                w, h, pixel_size, expected);
        return 0;
    }
    static idk_shm_cache_t s_shm_cache;
    static int s_cached_fd = -1;

    if (shm_fd != s_cached_fd) {
        idk_shm_cache_map(&s_shm_cache, shm_fd);
        if (s_cached_fd >= 0) close(s_cached_fd);
        s_cached_fd = shm_fd;
    }

    uint32_t buf_size = pixel_size;
    uint8_t *buf = (uint8_t*)s_shm_cache.map + (buffer_idx * buf_size);

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
    /* RGBA8 for both straight/premultiplied — blend mode differs at render time */
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

    return g_tex[g_tex_idx];
}

int idk_compositor_egl_render(void) {
    accept_client();
    if (!g_tp.ready) return -1;

    /* Poll for new frame — keep newest only */
    int processed = 0;

    while (1) {
        idk_frame_header_t hdr;
        int fds[4], nfd = 0;
        int rc = idk_tp_recv(&g_tp, &hdr, fds, &nfd);
        if (rc <= 0) {
            if (rc < 0) {
                IDK_LOG("comp", "client disconnected, resetting\n");
                idk_tp_destroy(&g_tp);
                g_tp.ready = false;
            }
            break;
        }

        int dmabuf_fd = (nfd > 0) ? fds[0] : -1;

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
            /* DMABUF frame — try two import paths:
             *   1. EGL (eglCreateImageKHR + glEGLImageTargetTexStorageEXT)
             *      for EGL apps (host has current EGL display).
             *   2. GL_EXT_memory_object (MangoHud-style, OPAQUE_FD trick)
             *      for GLX apps (no current EGL display). Works on Mesa.
             * If both fail, reject with ACK=1 → webview falls back to SHM. */

            /* Ensure EGL functions are resolved. */
            if (!fn_eglGetCurrentDisplay) {
                resolve_egl_functions();
            }

            GLuint tex = 0;
            EGLImageKHR img = 0;
            int fd_consumed = 0;  /* set if GL driver took fd ownership */

            /* Try EGL path first (works for EGL apps via current display) */
            tex = egl_dmabuf_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                         hdr.stride, hdr.fourcc,
                                         hdr.modifier, &img);

            if (tex == 0) {
                /* EGL path failed — try GL_EXT_memory_object MangoHud-style
                 * fallback. This works on Mesa even for EGL apps when the
                 * EGL driver doesn't support the specific modifier. */
                IDK_LOG("comp", "EGL dmabuf import failed, trying GL_EXT_memory_object (MangoHud) path\n");
                tex = gl_dmabuf_to_texture(dmabuf_fd, hdr.width, hdr.height,
                                            hdr.stride, hdr.fourcc);
                if (tex) {
                    IDK_LOG("comp", "GL_EXT_memory_object dmabuf import OK\n");
                    /* GL_EXT_memory_object dup'd the fd internally (driver
                     * owns the dup'd fd). Original dmabuf_fd is no longer
                     * needed — close it now. */
                    close(dmabuf_fd);
                    fd_consumed = 0;  /* fd already closed, don't track */
                } else {
                    IDK_ERR("comp", "GL_EXT_memory_object dmabuf import also failed\n");
                }
            } else {
                /* EGL path: eglCreateImageKHR kept fd alive via EGLImage,
                 * so original dmabuf_fd must be kept open (tracked for cleanup). */
                fd_consumed = 1;
            }

            if (tex == 0) {
                /* Both paths failed — reject, force SHM */
                IDK_LOG("comp", "DMABUF import failed (EGL + GL_EXT_memory_object) — rejecting, forcing SHM\n");
                close(dmabuf_fd);
                processed = -1;  /* ACK=1 → webview falls back to SHM */
            } else {
                int back = 1 - g_tex_idx;
                /* Replacing previous slot: delete its texture AND any
                 * DMABUF backing (EGLImage + fd) so we don't leak. */
                if (g_tex[back]) glDeleteTextures(1, &g_tex[back]);
                release_dmabuf_backing(back);
                /* Install new texture + keep its backing alive. */
                g_tex[back] = tex;
                g_tex_img[back] = img;
                /* For EGL path: keep fd open (EGLImage references it).
                 * For GL_EXT_memory_object path: close fd (driver dup'd it). */
                g_tex_dmabuf_fd[back] = fd_consumed ? dmabuf_fd : -1;
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
        idk_ack_msg_t ack_msg;
        idk_comp_build_ack(&ack_msg, ack,
                           g_game_w, g_game_h,
                           &g_size_pending, &g_last_resize_ts,
                           0, "comp");
        idk_tp_send_ack(&g_tp, &ack_msg);
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

    /* Determine display: prefer the host's current EGL display (for EGL apps).
     * For GLX apps (no current EGL display), the caller should fall back to
     * gl_dmabuf_to_texture (GL_EXT_memory_object MangoHud-style path).
     * EGLImage created on a separate compositor EGLDisplay cannot be bound
     * to a texture in the host's GLX context (Mesa driver crashes inside
     * glEGLImageTargetTexStorageEXT when the EGLImage's display doesn't
     * match the current context's display). */
    EGLDisplay egl_dpy = EGL_NO_DISPLAY;
    if (fn_eglGetCurrentDisplay) {
        egl_dpy = fn_eglGetCurrentDisplay();
    }
    if (egl_dpy == EGL_NO_DISPLAY) {
        /* GLX host — caller handles via gl_dmabuf_to_texture fallback */
        IDK_LOG("comp", "egl_dmabuf_to_texture: no current EGL display (GLX host?)\n");
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
    int ai_nom = ai;  /* index before modifier attrs, used for retry */
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
        EGLint egl_err = fn_eglGetError ? fn_eglGetError() : 0;
        IDK_LOG("comp", "eglCreateImageKHR failed: 0x%04X (dpy=%p) — retrying without modifier\n",
                (unsigned int)egl_err, (void*)egl_dpy);

        /* Retry without modifier — some EGL drivers can't import tiled/
         * compressed dmabufs but accept the same fd without modifier attr
         * (driver falls back to linear interpretation). */
        if (modifier != 0 && modifier != DRM_FORMAT_MOD_INVALID) {
            /* Retry without modifier by terminating the list before
             * the modifier attributes and re-adding EGL_NONE. */
            attrs[ai_nom] = EGL_NONE;
            img = fn_eglCreateImageKHR(
                egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
            if (img == EGL_NO_IMAGE_KHR) {
                egl_err = fn_eglGetError ? fn_eglGetError() : 0;
                IDK_ERR("comp", "eglCreateImageKHR retry (no modifier) also failed: 0x%04X\n",
                        (unsigned int)egl_err);
            } else {
                IDK_LOG("comp", "eglCreateImageKHR retry (no modifier) OK — imported as linear\n");
            }
        }

        if (img == EGL_NO_IMAGE_KHR) {
            return 0;
        }
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    IDK_LOG("comp", "egl_dmabuf_to_texture: glGenTextures tex=%u (dpy=%p host_dpy=%p)\n",
            tex, (void*)egl_dpy,
            (void*)(fn_eglGetCurrentDisplay ? fn_eglGetCurrentDisplay() : NULL));

    /* Resolve EGL image binding functions up front */
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

    /* TexStorageEXT first (desktop GL — proper EGL_image_storage path),
     * fall back to Texture2DOES (GLES-compatible but may crash on some
     * desktop GL drivers — only use as last resort). */
    GLboolean ok = GL_FALSE;
    GLenum err = GL_NO_ERROR;
    static int s_texstorage_success_logged = 0;
    static int s_texstorage_failed = 0;

    if (fn_glEGLImageTargetTexStorageEXT) {
        while (glGetError() != GL_NO_ERROR) {}  /* drain */
        fn_glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, (GLeglImage)img, NULL);
        err = glGetError();
        if (err == GL_NO_ERROR) {
            ok = GL_TRUE;
            if (!s_texstorage_success_logged) {
                IDK_LOG("comp", "EGL image bound via TexStorageEXT (will not re-log)\n");
                s_texstorage_success_logged = 1;
            }
        } else {
            s_texstorage_failed = 1;
            IDK_LOG("comp", "TexStorageEXT failed (0x%04X), trying Texture2DOES\n", err);
        }
    }

    /* Texture2DOES fallback — only if TexStorageEXT unavailable or failed.
     * On desktop GL, glEGLImageTargetTexture2DOES may not work correctly
     * (it's a GLES extension); only use if we have no better option. */
    if (!ok && fn_glEGLImageTargetTexture2DOES) {
        while (glGetError() != GL_NO_ERROR) {}  /* drain */
        fn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImage)img);
        err = glGetError();
        if (err == GL_NO_ERROR) {
            ok = GL_TRUE;
            IDK_LOG("comp", "EGL image bound via Texture2DOES (fallback)\n");
        }
    }
    if (err != GL_NO_ERROR) {
        IDK_ERR("comp", "EGL image import failed: 0x%04X\n", err);
    }

    if (!ok || err != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        if (fn_eglDestroyImageKHR) fn_eglDestroyImageKHR(egl_dpy, img);
        return 0;
    }

    /* If Texture2DOES fallback was used, set TEXTURE_IMMUTABLE_FORMAT=0
     * via glTexParameteri to avoid driver confusion on subsequent binds. */
    if (s_texstorage_failed && !s_texstorage_success_logged) {
        /* One-time warning */
        IDK_LOG("comp", "WARNING: using Texture2DOES fallback (TexStorageEXT failed) — "
                "texture may not be stable across GL contexts\n");
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Keep EGLImage alive — Mesa's TexStorageEXT doesn't retain a ref */
    if (out_img) *out_img = img;

    return tex;
}

void idk_compositor_egl_render_overlay(int x, int y, uint32_t w, uint32_t h) {
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
            IDK_LOG("comp", "program %u invalidated (is_prog=%d link=%d) — re-initializing shaders (attempt %d)\n",
                    g_program, (int)is_prog, (int)link_status, s_reinit_count);
            /* Clear g_program so init creates a fresh one.
             * Don't call glDeleteProgram — the ID may belong to the host. */
            g_program = 0;
            if (idk_compositor_egl_init_gl() != 0) {
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
        IDK_LOG("comp", "pre-draw diag: tex[%d]=%u valid=%d prog=%u link=%d frame=%ux%u fb=%dx%d\n",
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
                IDK_ERR("comp", "GL error after draw: 0x%x (tex[%d]=%u fb=%dx%d frame=%ux%u, occurrence %d)\n",
                        err, g_tex_idx, g_tex[g_tex_idx], fb_w, fb_h,
                        g_frame_w, g_frame_h, g_draw_err_count);
            }
            /* Invalidate texture after 5 consecutive errors */
            if (g_draw_err_count == 5) {
                GLuint dead = g_tex[g_tex_idx];
                IDK_ERR("comp", "tex[%d]=%u failing repeatedly — deleting + marking invalid\n",
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

int idk_compositor_egl_has_overlay(void) {
    return g_has_frame;
}

void idk_compositor_egl_shutdown(void) {
    /* SHM cache is function-scoped static in shm_to_texture — OS reclaims */
    if (g_dmabuf_fd >= 0) {
        close(g_dmabuf_fd);
        g_dmabuf_fd = -1;
    }

    idk_tp_destroy(&g_tp);

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
int idk_compositor_egl_init_gl(void) {
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
