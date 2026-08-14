#include "internal.h"
#include "rhi_texture_extractor.h"
#include "webview.h"

#include <QOpenGLContext>
#include <dlfcn.h>

#include "core/log.h"

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
    s_fn_glEGLImageTargetTexture2DOES = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  }
  if (!s_fn_glEGLImageTargetTexStorageEXT) {
    s_fn_glEGLImageTargetTexStorageEXT = (void *)eglGetProcAddress("glEGLImageTargetTexStorageEXT");
  }
}

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
