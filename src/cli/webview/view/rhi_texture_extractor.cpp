#include "rhi_texture_extractor.h"
#include "core/log.h"
#include "public/idk_fs.h"
#include "webview.h"

#include <EGL/eglext.h>
#include <QDateTime>
#include <QOpenGLContext>
#include <dlfcn.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wsfinae-incomplete"
#include <QtGui/private/qrhi_p.h>
#pragma GCC diagnostic pop

// Static GL helper pointers

void *RhiTextureExtractor::s_fn_glFenceSync = nullptr;
void *RhiTextureExtractor::s_fn_glClientWaitSync = nullptr;
void *RhiTextureExtractor::s_fn_glDeleteSync = nullptr;
void *RhiTextureExtractor::s_fn_glGenFramebuffers = nullptr;
void *RhiTextureExtractor::s_fn_glBindFramebuffer = nullptr;
void *RhiTextureExtractor::s_fn_glFramebufferTexture2D = nullptr;
void *RhiTextureExtractor::s_fn_glBlitFramebuffer = nullptr;
void *RhiTextureExtractor::s_fn_glReadPixels = nullptr;
void *RhiTextureExtractor::s_fn_glDeleteFramebuffers = nullptr;
void *RhiTextureExtractor::s_fn_glEGLImageTargetTexture2DOES = nullptr;
void *RhiTextureExtractor::s_fn_glEGLImageTargetTexStorageEXT = nullptr;

RhiTextureExtractor::RhiTextureExtractor(WebView *view) : m_view(view) {}

RhiTextureExtractor::~RhiTextureExtractor() {}

// Fence sync

void RhiTextureExtractor::resolveFenceGL() {
  if (s_fn_glFenceSync)
    return;
  void *lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (!lib)
    lib = dlopen("libOpenGL.so.0", RTLD_NOW);
  if (lib) {
    s_fn_glFenceSync = dlsym(lib, "glFenceSync");
    s_fn_glClientWaitSync = dlsym(lib, "glClientWaitSync");
    s_fn_glDeleteSync = dlsym(lib, "glDeleteSync");
  }
}

bool RhiTextureExtractor::fenceSyncGL() {
  resolveFenceGL();
  if (s_fn_glFenceSync && s_fn_glClientWaitSync && s_fn_glDeleteSync) {
    typedef void *(*PFN_glFenceSync)(unsigned int, unsigned int);
    typedef unsigned int (*PFN_glClientWaitSync)(void *, unsigned int, uint64_t);
    typedef void (*PFN_glDeleteSync)(void *);

    void *fence = ((PFN_glFenceSync)s_fn_glFenceSync)(0x9117, 0);
    if (fence) {
      glFlush();
      unsigned int result = ((PFN_glClientWaitSync)s_fn_glClientWaitSync)(fence, 0x00000001, 1000000000ULL);
      ((PFN_glDeleteSync)s_fn_glDeleteSync)(fence);
      return (result == 0x911A || result == 0x911C);
    }
  }
  glFinish();
  return true;
}

// FBO resolution

void RhiTextureExtractor::resolveFBOGL() {
  if (s_fn_glGenFramebuffers)
    return;
  void *lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (!lib)
    lib = dlopen("libOpenGL.so.0", RTLD_NOW);
  if (lib) {
    s_fn_glGenFramebuffers = dlsym(lib, "glGenFramebuffers");
    s_fn_glBindFramebuffer = dlsym(lib, "glBindFramebuffer");
    s_fn_glFramebufferTexture2D = dlsym(lib, "glFramebufferTexture2D");
    s_fn_glBlitFramebuffer = dlsym(lib, "glBlitFramebuffer");
    s_fn_glReadPixels = dlsym(lib, "glReadPixels");
    s_fn_glDeleteFramebuffers = dlsym(lib, "glDeleteFramebuffers");
  }
}

// Image binding (EGLImage → GL texture)

void RhiTextureExtractor::resolveImageBindGL() {
  if (s_fn_glEGLImageTargetTexture2DOES)
    return;
  void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
  if (!lib)
    lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (lib) {
    s_fn_glEGLImageTargetTexture2DOES = dlsym(lib, "glEGLImageTargetTexture2DOES");
    s_fn_glEGLImageTargetTexStorageEXT = dlsym(lib, "glEGLImageTargetTexStorageEXT");
  }
  if (!s_fn_glEGLImageTargetTexture2DOES) {
    // eglGetProcAddress is the reliable source in a Qt EGL context
    s_fn_glEGLImageTargetTexture2DOES = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  }
  if (!s_fn_glEGLImageTargetTexStorageEXT) {
    s_fn_glEGLImageTargetTexStorageEXT = (void *)eglGetProcAddress("glEGLImageTargetTexStorageEXT");
  }
}

// Shared context

bool RhiTextureExtractor::ensureDmaBufSharedCtx() {
  if (!m_view->m_needSharedCtx)
    return true;

  EGLContext qtEglCtx = EGL_NO_CONTEXT;
  EGLDisplay qtEglDpy = EGL_NO_DISPLAY;
  EGLConfig qtEglConfig = nullptr;

  if (auto *qw = qobject_cast<QQuickWidget *>(m_view->focusProxy())) {
    if (auto *window = qw->quickWindow()) {
      auto *rif = window->rendererInterface();
      if (rif) {
        auto *qtCtx =
            static_cast<QOpenGLContext *>(rif->getResource(window, QSGRendererInterface::OpenGLContextResource));
        if (qtCtx) {
          auto *eglIface = qtCtx->nativeInterface<QNativeInterface::QEGLContext>();
          if (eglIface) {
            qtEglCtx = eglIface->nativeContext();
            qtEglDpy = eglIface->display();
          }
        }
      }
    }
  }

  if (qtEglCtx == EGL_NO_CONTEXT)
    return false;

  EGLint qtConfigId = 0;
  if (qtEglDpy != EGL_NO_DISPLAY) {
    eglQueryContext(qtEglDpy, qtEglCtx, EGL_CONFIG_ID, &qtConfigId);
    if (qtConfigId > 0) {
      EGLint cfg_attribs[] = {EGL_CONFIG_ID, qtConfigId, EGL_NONE};
      EGLint ncfg = 0;
      eglChooseConfig(qtEglDpy, cfg_attribs, &qtEglConfig, 1, &ncfg);
      if (ncfg > 0 && qtEglConfig) {
        m_view->m_eglDpy = qtEglDpy;
        m_view->m_eglConfig = qtEglConfig;
        if (m_view->m_eglSurf != EGL_NO_SURFACE) {
          eglDestroySurface(m_view->m_eglDpy, m_view->m_eglSurf);
          m_view->m_eglSurf = EGL_NO_SURFACE;
        }
        static const EGLint pbuf_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        m_view->m_eglSurf = eglCreatePbufferSurface(m_view->m_eglDpy, m_view->m_eglConfig, pbuf_attribs);
      }
    }
  }

  if (m_view->m_eglCtx != EGL_NO_CONTEXT)
    eglDestroyContext(m_view->m_eglDpy, m_view->m_eglCtx);

  static const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  m_view->m_eglCtx = eglCreateContext(m_view->m_eglDpy, m_view->m_eglConfig, qtEglCtx, ctx_attribs);
  if (m_view->m_eglCtx == EGL_NO_CONTEXT) {
    EGLint err = eglGetError();
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      IDK_LOG("webview-qt",
              "ensureDmaBufSharedCtx: eglCreateContext failed (eglError=0x%x) - "
              "DMABUF disabled, using SHM fallback\n",
              err);
    }
    m_view->m_useDmaBuf = false;
    return false;
  }

  m_view->m_needSharedCtx = false;
  IDK_LOG("webview-qt", "DMABUF context now shared with Qt (config_id=%d)\n", qtConfigId);
  return true;
}

// DMABUF dispatcher

bool RhiTextureExtractor::tryExportDMABuf() {
  QQuickWidget *qw = qobject_cast<QQuickWidget *>(m_view->focusProxy());
  if (!qw)
    return false;
  QQuickWindow *window = qw->quickWindow();
  if (!window)
    return false;
  auto *rif = window->rendererInterface();
  if (!rif)
    return false;

  auto api = rif->graphicsApi();
  switch (api) {
  case QSGRendererInterface::OpenGL:
    return tryExportDMABufOpenGL();
#ifdef IDK_HAVE_VULKAN
  case QSGRendererInterface::Vulkan:
    return tryExportDMABufVulkan();
#endif
  default:
    m_view->m_dmaBufFailed = true;
    return false;
  }
}

// OpenGL DMABUF export

bool RhiTextureExtractor::tryExportDMABufOpenGL() {
  if (m_view->m_needSharedCtx && !ensureDmaBufSharedCtx())
    return false;

  QQuickWidget *qw = qobject_cast<QQuickWidget *>(m_view->focusProxy());
  if (!qw)
    return false;
  QQuickWindow *window = qw->quickWindow();
  if (!window)
    return false;
  auto *rif = window->rendererInterface();
  if (!rif)
    return false;

  EGLDisplay savedDpy = eglGetCurrentDisplay();
  EGLContext savedCtx = eglGetCurrentContext();
  EGLSurface savedRead = eglGetCurrentSurface(EGL_READ);
  EGLSurface savedDraw = eglGetCurrentSurface(EGL_DRAW);

  if (!eglMakeCurrent(m_view->m_eglDpy, m_view->m_eglSurf, m_view->m_eglSurf, m_view->m_eglCtx)) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: eglMakeCurrent failed\n");
    return false;
  }

  /* GPU sync before accessing Qt's texture via shared context */
  if (!fenceSyncGL()) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: fence sync failed\n");
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }

  EGLDisplay exportDpy = m_view->m_eglDpy;
  EGLContext exportCtx = m_view->m_eglCtx;

  auto *rhiRt = reinterpret_cast<QRhiTextureRenderTarget *>(
      rif->getResource(window, QSGRendererInterface::RhiRedirectRenderTarget));
  if (!rhiRt) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: RhiRedirectRenderTarget null\n");
    m_view->m_dmaBufFailed = true;
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }

  auto desc = rhiRt->description();
  if (desc.colorAttachmentCount() == 0) {
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }
  const QRhiColorAttachment *ca = desc.colorAttachmentAt(0);
  if (!ca || !ca->texture()) {
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }

  QRhiTexture::NativeTexture native = ca->texture()->nativeTexture();
  GLuint texId = static_cast<GLuint>(native.object);
  if (!texId) {
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }

  int w = ca->texture()->pixelSize().width();
  int h = ca->texture()->pixelSize().height();
  if (w <= 0 || h <= 0) {
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }

  /* Recreate copy textures on size/ownership change. Two GPU blits:
   *   m_dmaTex (tiled, renderable) ← Qt texture
   *   m_dmaTexLinear (LINEAR dmabuf) ← m_dmaTex
   * so the exported fd is LINEAR — the overlay's GL_EXT_memory_object
   * import has no modifier param. No CPU/RAM copy anywhere. */
  if (m_view->m_dmaTex == 0 || m_view->m_dmaTexW != w || m_view->m_dmaTexH != h) {
    if (m_view->m_dmaEglImgLinear != EGL_NO_IMAGE_KHR) {
      eglDestroyImage(exportDpy, m_view->m_dmaEglImgLinear);
      m_view->m_dmaEglImgLinear = EGL_NO_IMAGE_KHR;
    }
    if (m_view->m_dmaExportFd >= 0) {
      ::close(m_view->m_dmaExportFd);
      m_view->m_dmaExportFd = -1;
    }
    m_view->m_dmaExportFourcc = 0;
    m_view->m_dmaExportStride = 0;
    m_view->m_dmaExportModifier = 0;

    if (m_view->m_dmaTexLinear)
      glDeleteTextures(1, &m_view->m_dmaTexLinear);
    m_view->m_dmaTexLinear = 0;
    if (m_view->m_dmaTex)
      glDeleteTextures(1, &m_view->m_dmaTex);
    m_view->m_dmaTex = 0;

    /* Staging dmabuf via gbm. i915's glTexStorageMem2DEXT creates a
     * TILED texture (render-target default) and does NOT convert layout
     * — importing a LINEAR buffer reads it as tiled (wrong). Use the
     * driver's default tiled buffer so the texture layout matches. */
    static gbm_device *s_gbm = nullptr;
    if (!s_gbm) {
      int rfd = open("/dev/dri/renderD128", O_RDWR);
      if (rfd < 0)
        rfd = open("/dev/dri/renderD129", O_RDWR);
      if (rfd >= 0)
        s_gbm = gbm_create_device(rfd);
    }
    int linearFd = -1;
    uint32_t linearStride = 0;
    uint64_t realMod = 0;
    if (s_gbm) {
      gbm_bo *bo = gbm_bo_create(s_gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING);
      if (bo) {
        linearFd = gbm_bo_get_fd(bo);
        linearStride = gbm_bo_get_stride(bo);
        realMod = gbm_bo_get_modifier(bo);
        IDK_LOG("webview-qt", "gbm bo: fd=%d stride=%u mod=0x%llx\n", linearFd, linearStride,
                (unsigned long long)realMod);
        gbm_bo_destroy(bo); /* fd keeps the buffer alive */
      } else {
        IDK_LOG("webview-qt", "gbm_bo_create(RENDERING) returned NULL\n");
      }
    }
    if (linearFd >= 0 && linearStride > 0) {
      EGLAttrib attrs[] = {EGL_WIDTH,
                           w,
                           EGL_HEIGHT,
                           h,
                           EGL_LINUX_DRM_FOURCC_EXT,
                           GBM_FORMAT_ABGR8888,
                           EGL_DMA_BUF_PLANE0_FD_EXT,
                           linearFd,
                           EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                           0,
                           EGL_DMA_BUF_PLANE0_PITCH_EXT,
                           (EGLAttrib)linearStride,
                           EGL_NONE};
      m_view->m_dmaEglImgLinear = eglCreateImage(exportDpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
      if (m_view->m_dmaEglImgLinear != EGL_NO_IMAGE_KHR) {
        resolveImageBindGL();
        glGenTextures(1, &m_view->m_dmaTexLinear);
        glBindTexture(GL_TEXTURE_2D, m_view->m_dmaTexLinear);
        bool bound = false;
        if (s_fn_glEGLImageTargetTexStorageEXT) {
          ((void (*)(GLenum, void *, const GLint *))s_fn_glEGLImageTargetTexStorageEXT)(
              GL_TEXTURE_2D, (void *)m_view->m_dmaEglImgLinear, NULL);
          bound = (glGetError() == GL_NO_ERROR);
        }
        if (!bound && s_fn_glEGLImageTargetTexture2DOES) {
          ((void (*)(GLenum, void *))s_fn_glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D,
                                                                        (void *)m_view->m_dmaEglImgLinear);
          bound = (glGetError() == GL_NO_ERROR);
        }
        if (bound) {
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
          m_view->m_dmaExportFd = linearFd;
          m_view->m_dmaExportFourcc = GBM_FORMAT_ABGR8888;
          m_view->m_dmaExportStride = linearStride;
          m_view->m_dmaExportModifier = realMod;
          IDK_LOG("webview-qt", "STAGING dmabuf: fd=%d stride=%u fourcc=0x%x mod=0x%llx\n", linearFd, linearStride,
                  (unsigned)GBM_FORMAT_ABGR8888, (unsigned long long)realMod);
        } else {
          glDeleteTextures(1, &m_view->m_dmaTexLinear);
          m_view->m_dmaTexLinear = 0;
          eglDestroyImage(exportDpy, m_view->m_dmaEglImgLinear);
          m_view->m_dmaEglImgLinear = EGL_NO_IMAGE_KHR;
        }
      }
      if (m_view->m_dmaTexLinear == 0)
        ::close(linearFd);
    }

    /* First-stage copy texture: tiled, renderable, blit target for Qt.
     * Fallback export (tiled) uses this when no linear buffer exists. */
    glGenTextures(1, &m_view->m_dmaTex);
    glBindTexture(GL_TEXTURE_2D, m_view->m_dmaTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_view->m_dmaTexW = w;
    m_view->m_dmaTexH = h;
    m_view->m_dmaBufId++;
    if (m_view->m_dmaBufId == 0)
      m_view->m_dmaBufId = 1;
  }

  resolveFBOGL();
  typedef void (*PFN_glGenFramebuffers)(GLsizei, GLuint *);
  typedef void (*PFN_glBindFramebuffer)(GLenum, GLuint);
  typedef void (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  typedef void (*PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
  typedef void (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);

  if (!s_fn_glGenFramebuffers || !s_fn_glBindFramebuffer || !s_fn_glFramebufferTexture2D || !s_fn_glBlitFramebuffer ||
      !s_fn_glDeleteFramebuffers) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: FBO functions not available\n");
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }

  GLuint readFbo = 0, drawFbo = 0;
  ((PFN_glGenFramebuffers)s_fn_glGenFramebuffers)(1, &readFbo);
  ((PFN_glGenFramebuffers)s_fn_glGenFramebuffers)(1, &drawFbo);
  ((PFN_glBindFramebuffer)s_fn_glBindFramebuffer)(0x8CA8, readFbo);
  ((PFN_glFramebufferTexture2D)s_fn_glFramebufferTexture2D)(0x8CA8, 0x8CE0, 0x0DE1, texId, 0);
  /* Stage 1: Qt texture → tiled copy texture (GPU). */
  ((PFN_glBindFramebuffer)s_fn_glBindFramebuffer)(0x8CA9, drawFbo);
  ((PFN_glFramebufferTexture2D)s_fn_glFramebufferTexture2D)(0x8CA9, 0x8CE0, 0x0DE1, m_view->m_dmaTex, 0);
  ((PFN_glBlitFramebuffer)s_fn_glBlitFramebuffer)(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2600);
  /* Stage 2: tiled copy → linear dmabuf texture (GPU). */
  if (m_view->m_dmaTexLinear != 0) {
    ((PFN_glFramebufferTexture2D)s_fn_glFramebufferTexture2D)(0x8CA9, 0x8CE0, 0x0DE1, m_view->m_dmaTexLinear, 0);
    ((PFN_glBlitFramebuffer)s_fn_glBlitFramebuffer)(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2600);
    typedef void (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    static unsigned char *s_px = nullptr;
    if (!s_px)
      s_px = (unsigned char *)malloc(16);
    memset(s_px, 0, 16);
    ((PFN_glReadPixels)s_fn_glReadPixels)(0, 0, 2, 2, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, s_px);
    static int s_readback_count = 0;
    if (s_readback_count++ % 30 == 0) {
      /* Read the actual dma-buf, not the GL texture (GL may hold the
       * data internally while the buffer stays empty). */
      size_t bufsz = (size_t)m_view->m_dmaExportStride * h;
      void *map = mmap(NULL, bufsz, PROT_READ, MAP_SHARED, m_view->m_dmaExportFd, 0);
      if (map != MAP_FAILED) {
        const unsigned char *b = (const unsigned char *)map;
        IDK_LOG(
            "webview-qt",
            "linear2 buf: off0=%02x%02x%02x%02x off4=%02x%02x%02x%02x off8=%02x%02x%02x%02x off12=%02x%02x%02x%02x\n",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        /* row 1 (stride offset) and row 2 */
        uint32_t st = m_view->m_dmaExportStride;
        IDK_LOG("webview-qt",
                "linear2 buf rows: r1=%02x%02x%02x%02x r2=%02x%02x%02x%02x r3=%02x%02x%02x%02x r4=%02x%02x%02x%02x\n",
                b[st], b[st + 1], b[st + 2], b[st + 3], b[2 * st], b[2 * st + 1], b[2 * st + 2], b[2 * st + 3],
                b[3 * st], b[3 * st + 1], b[3 * st + 2], b[3 * st + 3], b[4 * st], b[4 * st + 1], b[4 * st + 2],
                b[4 * st + 3]);
        munmap(map, bufsz);
      } else {
        IDK_LOG("webview-qt", "linear2 mmap fail\n");
      }
    }
  }
  ((PFN_glBindFramebuffer)s_fn_glBindFramebuffer)(0x8D40, 0);
  ((PFN_glDeleteFramebuffers)s_fn_glDeleteFramebuffers)(1, &readFbo);
  ((PFN_glDeleteFramebuffers)s_fn_glDeleteFramebuffers)(1, &drawFbo);

  /* Fence sync after blit */
  if (!fenceSyncGL()) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: blit fence sync failed\n");
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }

  /* Export our texture as dmabuf. If the linear staging path worked
   * (gbm above), m_dmaExportFd is already the linear dmabuf fd — skip
   * the EGL export query (which would export the tiled texture). */
  glBindTexture(GL_TEXTURE_2D, m_view->m_dmaTex);

  if (m_view->m_dmaExportFd < 0) {
    m_view->m_dmaEglImg =
        eglCreateImage(exportDpy, exportCtx, EGL_GL_TEXTURE_2D,
                       reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(m_view->m_dmaTex)), nullptr);
    if (!m_view->m_dmaEglImg) {
      IDK_LOG("webview-qt", "tryExportDMABufOpenGL: eglCreateImage failed (0x%x)\n", eglGetError());
      glBindTexture(GL_TEXTURE_2D, 0);
      if (savedDpy != EGL_NO_DISPLAY)
        eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
      return false;
    }

    EGLint fourcc = 0, nfd = 0;
    EGLuint64KHR modifier = 0;
    int fds[4] = {-1, -1, -1, -1};
    EGLint strides[4] = {0};
    EGLint offsets[4] = {0};

    auto qFn = reinterpret_cast<EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLuint64KHR *)>(
        m_view->m_queryFn);
    auto eFn = reinterpret_cast<EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLint *)>(
        m_view->m_exportFn);

    if (!qFn || !eFn || !qFn(exportDpy, m_view->m_dmaEglImg, &fourcc, &nfd, &modifier) ||
        !eFn(exportDpy, m_view->m_dmaEglImg, fds, strides, offsets) || nfd < 1 || fds[0] < 0) {
      IDK_LOG("webview-qt", "tryExportDMABufOpenGL: export query failed\n");
      /* Close any fds the driver may have populated before failing.
       * Per spec eFn shouldn't write fds on failure, but driver
       * bugs can cause partial population - without this loop,
       * those fds leak. fds[] was initialized to {-1,-1,-1,-1}
       * so we only close the ones that were actually written. */
      for (int i = 0; i < 4; i++) {
        if (fds[i] >= 0)
          ::close(fds[i]);
        fds[i] = -1;
      }
      eglDestroyImage(exportDpy, m_view->m_dmaEglImg);
      m_view->m_dmaEglImg = EGL_NO_IMAGE_KHR;
      glBindTexture(GL_TEXTURE_2D, 0);
      if (savedDpy != EGL_NO_DISPLAY)
        eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
      return false;
    }
    m_view->m_dmaExportFd = fds[0];
    m_view->m_dmaExportFourcc = static_cast<uint32_t>(fourcc);
    m_view->m_dmaExportStride = static_cast<uint32_t>(strides[0]);
    m_view->m_dmaExportModifier = modifier;
    for (int i = 1; i < nfd && i < 4; i++)
      if (fds[i] >= 0)
        ::close(fds[i]);
  }

  {
    idk_frame_header_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.width = static_cast<uint32_t>(w);
    frame.height = static_cast<uint32_t>(h);
    frame.flags = IDK_FRAME_FLAG_VISIBLE;
    frame.nfd = 1;
    frame.stride = m_view->m_dmaExportStride;
    frame.fourcc = m_view->m_dmaExportFourcc;
    frame.modifier = m_view->m_dmaExportModifier;
    frame.buf_id = m_view->m_dmaBufId;

    /* Pass the original dmabuf fd (not a dup) so the SHM backend can
     * write its number into the shared page - the fd stays open as
     * long as m_dmaExportFd is valid (across frames of same size).
     * The socket backend (SCM_RIGHTS) transfers the fd atomically. */
    int fds_arr[4] = {m_view->m_dmaExportFd, -1, -1, -1};
    int rc = idk_fs_send_dma_buf(fds_arr, &frame);

    if (rc == 0) {
      m_view->m_buffer = (m_view->m_buffer + 1) % 2;
      m_view->m_pending = true;
      m_view->m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
      emit m_view->frameSent();
    } else {
      /* Throttle log: only first failure + every 60th after.
       * If transport disconnected, caller (doRenderAndSend SHM path)
       * will detect via idk_fs_is_connected() and stop heartbeat. */
      static int s_dmabuf_send_fail = 0;
      s_dmabuf_send_fail++;
      if (s_dmabuf_send_fail == 1 || s_dmabuf_send_fail % 60 == 0) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: idk_fs_send_dma_buf failed rc=%d (attempt %d, errno=%d: %s)\n",
                rc, s_dmabuf_send_fail, errno, strerror(errno));
      }
      if (s_dmabuf_send_fail > 5 && !idk_fs_is_connected()) {
        s_dmabuf_send_fail = 0;
      }
      glBindTexture(GL_TEXTURE_2D, 0);
      if (savedDpy != EGL_NO_DISPLAY)
        eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
      return false;
    }
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  if (savedDpy != EGL_NO_DISPLAY)
    eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
  return true;
}

// SHM via glReadPixels

bool RhiTextureExtractor::tryReadPixelsToSHM(unsigned char *shm, int w, int h) {
  if (m_view->m_needSharedCtx && !ensureDmaBufSharedCtx())
    return false;

  QQuickWidget *qw = qobject_cast<QQuickWidget *>(m_view->focusProxy());
  if (!qw)
    return false;
  QQuickWindow *window = qw->quickWindow();
  if (!window)
    return false;
  auto *rif = window->rendererInterface();
  if (!rif)
    return false;

  auto *rhiRt = reinterpret_cast<QRhiTextureRenderTarget *>(
      rif->getResource(window, QSGRendererInterface::RhiRedirectRenderTarget));
  if (!rhiRt)
    return false;

  auto desc = rhiRt->description();
  if (desc.colorAttachmentCount() == 0)
    return false;
  const QRhiColorAttachment *ca = desc.colorAttachmentAt(0);
  if (!ca || !ca->texture())
    return false;

  QRhiTexture::NativeTexture native = ca->texture()->nativeTexture();
  GLuint texId = static_cast<GLuint>(native.object);
  if (!texId)
    return false;

  EGLDisplay savedDpy = eglGetCurrentDisplay();
  EGLContext savedCtx = eglGetCurrentContext();
  EGLSurface savedRead = eglGetCurrentSurface(EGL_READ);
  EGLSurface savedDraw = eglGetCurrentSurface(EGL_DRAW);

  if (!eglMakeCurrent(m_view->m_eglDpy, m_view->m_eglSurf, m_view->m_eglSurf, m_view->m_eglCtx)) {
    IDK_LOG("webview-qt", "tryReadPixelsToSHM: eglMakeCurrent failed\n");
    return false;
  }

  if (!fenceSyncGL()) {
    IDK_LOG("webview-qt", "tryReadPixelsToSHM: fence sync failed\n");
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }

  resolveFBOGL();
  typedef void (*PFN_glGenFramebuffers)(GLsizei, GLuint *);
  typedef void (*PFN_glBindFramebuffer)(GLenum, GLuint);
  typedef void (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  typedef void (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
  typedef void (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);

  if (!s_fn_glGenFramebuffers || !s_fn_glBindFramebuffer || !s_fn_glFramebufferTexture2D || !s_fn_glReadPixels ||
      !s_fn_glDeleteFramebuffers) {
    if (savedDpy != EGL_NO_DISPLAY)
      eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
    return false;
  }

  GLuint readFbo = 0;
  ((PFN_glGenFramebuffers)s_fn_glGenFramebuffers)(1, &readFbo);
  ((PFN_glBindFramebuffer)s_fn_glBindFramebuffer)(0x8CA8, readFbo);
  ((PFN_glFramebufferTexture2D)s_fn_glFramebufferTexture2D)(0x8CA8, 0x8CE0, 0x0DE1, texId, 0);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  ((PFN_glReadPixels)s_fn_glReadPixels)(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, shm);

  ((PFN_glBindFramebuffer)s_fn_glBindFramebuffer)(0x8D40, 0);
  ((PFN_glDeleteFramebuffers)s_fn_glDeleteFramebuffers)(1, &readFbo);

  if (savedDpy != EGL_NO_DISPLAY)
    eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);

  m_view->m_framePremultiplied = true;
  return true;
}

// Vulkan DMABUF export

bool RhiTextureExtractor::tryExportDMABufVulkan() {
#ifdef IDK_HAVE_VULKAN
  if (!m_view->m_vk.resolved) {
    QQuickWindow *window = qobject_cast<QQuickWidget *>(m_view->focusProxy())->quickWindow();
    auto *rif = window->rendererInterface();
    m_view->initVulkan(rif, window);
    if (!m_view->m_vk.resolved)
      return false;
  }

  QQuickWidget *qw = qobject_cast<QQuickWidget *>(m_view->focusProxy());
  if (!qw)
    return false;
  QQuickWindow *window = qw->quickWindow();
  if (!window)
    return false;
  auto *rif = window->rendererInterface();
  if (!rif)
    return false;

  auto *rhiRt = reinterpret_cast<QRhiTextureRenderTarget *>(
      rif->getResource(window, QSGRendererInterface::RhiRedirectRenderTarget));
  if (!rhiRt) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: RhiRedirectRenderTarget null\n");
    return false;
  }

  auto desc = rhiRt->description();
  if (desc.colorAttachmentCount() == 0)
    return false;
  const QRhiColorAttachment *ca = desc.colorAttachmentAt(0);
  if (!ca || !ca->texture())
    return false;

  QRhiTexture::NativeTexture native = ca->texture()->nativeTexture();
  VkImage image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(native.object));
  VkImageLayout currentLayout = static_cast<VkImageLayout>(native.layout);
  if (!image) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: native VkImage is null\n");
    return false;
  }

  VkDevice dev = m_view->m_vk.device;
  QSize texSize = ca->texture()->pixelSize();
  uint32_t w = static_cast<uint32_t>(texSize.width());
  uint32_t h = static_cast<uint32_t>(texSize.height());
  VkDeviceSize bufSize = w * h * 4;

  VkBufferCreateInfo bufInfo = {};
  bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufInfo.size = bufSize;
  bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkBuffer buffer = VK_NULL_HANDLE;
  if (vkCreateBuffer(dev, &bufInfo, nullptr, &buffer) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkCreateBuffer failed\n");
    return false;
  }

  VkMemoryRequirements memReqs;
  vkGetBufferMemoryRequirements(dev, buffer, &memReqs);

  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(m_view->m_vk.physDev, &memProps);

  uint32_t memTypeIdx = UINT32_MAX;
  for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
    if (!(memReqs.memoryTypeBits & (1u << i)))
      continue;
    if (!(memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      continue;
    memTypeIdx = i;
    break;
  }
  if (memTypeIdx == UINT32_MAX) {
    vkDestroyBuffer(dev, buffer, nullptr);
    return false;
  }

  VkExportMemoryAllocateInfo exportInfo = {};
  exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkMemoryAllocateInfo allocInfo = {};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.pNext = &exportInfo;
  allocInfo.allocationSize = memReqs.size;
  allocInfo.memoryTypeIndex = memTypeIdx;

  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (vkAllocateMemory(dev, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkAllocateMemory failed\n");
    vkDestroyBuffer(dev, buffer, nullptr);
    return false;
  }
  /* Check vkBindBufferMemory return - if it fails, vkCmdCopyImageToBuffer
   * operates on an unbound buffer → UB. Previously the return value was
   * silently discarded. */
  if (vkBindBufferMemory(dev, buffer, memory, 0) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkBindBufferMemory failed\n");
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  VkCommandBufferAllocateInfo cmdAI = {};
  cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAI.commandPool = m_view->m_vk.cmdPool;
  cmdAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAI.commandBufferCount = 1;

  VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(dev, &cmdAI, &cmdBuf) != VK_SUCCESS) {
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  VkCommandBufferBeginInfo beginInfo = {};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  /* Check vkBeginCommandBuffer - if it fails, all subsequent vkCmd*
   * calls are no-ops or UB. Previously the return value was discarded. */
  if (vkBeginCommandBuffer(cmdBuf, &beginInfo) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkBeginCommandBuffer failed\n");
    vkFreeCommandBuffers(dev, m_view->m_vk.cmdPool, 1, &cmdBuf);
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  VkImageMemoryBarrier toTransfer = {};
  toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = currentLayout;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.image = image;
  toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toTransfer.subresourceRange.levelCount = 1;
  toTransfer.subresourceRange.layerCount = 1;

  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &toTransfer);

  VkBufferImageCopy copyRegion = {};
  copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copyRegion.imageSubresource.layerCount = 1;
  copyRegion.imageExtent.width = w;
  copyRegion.imageExtent.height = h;
  copyRegion.imageExtent.depth = 1;

  vkCmdCopyImageToBuffer(cmdBuf, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copyRegion);

  VkImageMemoryBarrier back = {};
  back.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  back.newLayout = currentLayout;
  back.image = image;
  back.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  back.subresourceRange.levelCount = 1;
  back.subresourceRange.layerCount = 1;

  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &back);

  /* Check vkEndCommandBuffer - if it fails, vkQueueSubmit will fail,
   * but the error path should free cmdBuf cleanly. Previously the
   * return value was discarded. */
  if (vkEndCommandBuffer(cmdBuf) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkEndCommandBuffer failed\n");
    vkFreeCommandBuffers(dev, m_view->m_vk.cmdPool, 1, &cmdBuf);
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  VkFenceCreateInfo fenceInfo = {};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  if (vkCreateFence(dev, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
    vkFreeCommandBuffers(dev, m_view->m_vk.cmdPool, 1, &cmdBuf);
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  VkSubmitInfo submitInfo = {};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmdBuf;

  if (vkQueueSubmit(m_view->m_vk.queue, 1, &submitInfo, fence) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkQueueSubmit failed\n");
    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, m_view->m_vk.cmdPool, 1, &cmdBuf);
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  /* Use a finite timeout (1 second) instead of UINT64_MAX. A GPU
   * hang (driver bug, TDR, lost device) would otherwise block the
   * webview forever - it freezes and cannot recover. The compositor's
   * VK path already uses a 1s timeout for its ring fences. On
   * timeout, treat as failure and clean up. */
  VkResult waitResult = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ULL);
  vkDestroyFence(dev, fence, nullptr);
  vkFreeCommandBuffers(dev, m_view->m_vk.cmdPool, 1, &cmdBuf);
  if (waitResult != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkWaitForFences %s (GPU hang?)\n",
            waitResult == VK_TIMEOUT ? "timed out" : "failed");
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  if (!m_view->m_vkGetMemoryFdKHR) {
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  VkMemoryGetFdInfoKHR fdInfo = {};
  fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  fdInfo.memory = memory;
  fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  int dmabufFd = -1;
  if (m_view->m_vkGetMemoryFdKHR(dev, &fdInfo, &dmabufFd) != VK_SUCCESS || dmabufFd < 0) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkGetMemoryFdKHR failed\n");
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  idk_frame_header_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.width = w;
  frame.height = h;
  frame.flags = IDK_FRAME_FLAG_VISIBLE;
  frame.nfd = 1;
  frame.stride = w * 4;
  frame.fourcc = 0x34324241;

  int fds[4] = {dmabufFd, -1, -1, -1};
  int rc = idk_fs_send_dma_buf(fds, &frame);
  ::close(dmabufFd);

  vkDestroyBuffer(dev, buffer, nullptr);
  vkFreeMemory(dev, memory, nullptr);

  if (rc == 0) {
    m_view->m_buffer = (m_view->m_buffer + 1) % 2;
    m_view->m_pending = true;
    m_view->m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
    emit m_view->frameSent();
    return true;
  }
#endif
  return false;
}
