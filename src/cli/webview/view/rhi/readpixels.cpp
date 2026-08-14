#include "internal.h"
#include "rhi_texture_extractor.h"
#include "webview.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wsfinae-incomplete"
#include <QtGui/private/qrhi_p.h>
#pragma GCC diagnostic pop

#include <unistd.h>

#include "core/log.h"

QRhiTexture *getRhiColorTexture(WebView *view, bool *outRhiRtMissing) {
  *outRhiRtMissing = false;
  QQuickWidget *qw = qobject_cast<QQuickWidget *>(view->focusProxy());
  if (!qw)
    return nullptr;
  QQuickWindow *window = qw->quickWindow();
  if (!window)
    return nullptr;
  auto *rif = window->rendererInterface();
  if (!rif)
    return nullptr;

  auto *rhiRt = reinterpret_cast<QRhiTextureRenderTarget *>(
      rif->getResource(window, QSGRendererInterface::RhiRedirectRenderTarget));
  if (!rhiRt) {
    *outRhiRtMissing = true;
    return nullptr;
  }

  auto desc = rhiRt->description();
  if (desc.colorAttachmentCount() == 0)
    return nullptr;
  const QRhiColorAttachment *ca = desc.colorAttachmentAt(0);
  if (!ca || !ca->texture())
    return nullptr;

  int w = ca->texture()->pixelSize().width();
  int h = ca->texture()->pixelSize().height();
  if (w <= 0 || h <= 0)
    return nullptr;
  return ca->texture();
}

bool RhiTextureExtractor::tryReadPixelsToSHM(unsigned char *shm, int w, int h) {
  if (m_view->m_needSharedCtx && !ensureDmaBufSharedCtx())
    return false;

  bool rhiRtMissing = false;
  QRhiTexture *tex = getRhiColorTexture(m_view, &rhiRtMissing);
  if (!tex)
    return false;

  QRhiTexture::NativeTexture native = tex->nativeTexture();
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
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
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
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
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

  restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);

  m_view->m_framePremultiplied = true;
  return true;
}

#ifdef IDK_HAVE_VULKAN
bool exportMemoryFd(VkDevice dev, VkDeviceMemory memory, PFN_vkGetMemoryFdKHR fn, int *outFd) {
  if (!fn)
    return false;

  VkMemoryGetFdInfoKHR fdInfo = {};
  fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  fdInfo.memory = memory;
  fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  int dmabufFd = -1;
  if (fn(dev, &fdInfo, &dmabufFd) != VK_SUCCESS || dmabufFd < 0) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkGetMemoryFdKHR failed\n");
    return false;
  }
  *outFd = dmabufFd;
  return true;
}
#endif

/* Export the copy texture as dmabuf via eglExportDMABUFImageMESA/EXT.
 * If the linear gbm staging path worked, m_dmaExportFd is already the
 * linear dmabuf fd — the EGL export query (which would export the tiled
 * texture) is skipped. */
bool exportDmaBufTexture(WebView *view, EGLDisplay dpy, EGLContext ctx) {
  glBindTexture(GL_TEXTURE_2D, view->m_dmaTex);

  if (view->m_dmaExportFd >= 0)
    return true;

  view->m_dmaEglImg = eglCreateImage(
      dpy, ctx, EGL_GL_TEXTURE_2D, reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(view->m_dmaTex)), nullptr);
  if (!view->m_dmaEglImg) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: eglCreateImage failed (0x%x)\n", eglGetError());
    glBindTexture(GL_TEXTURE_2D, 0);
    return false;
  }

  EGLint fourcc = 0, nfd = 0;
  EGLuint64KHR modifier = 0;
  int fds[4] = {-1, -1, -1, -1};
  EGLint strides[4] = {0};
  EGLint offsets[4] = {0};

  auto qFn = reinterpret_cast<EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLuint64KHR *)>(
      view->m_queryFn);
  auto eFn = reinterpret_cast<EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLint *)>(
      view->m_exportFn);

  if (!qFn || !eFn || !qFn(dpy, view->m_dmaEglImg, &fourcc, &nfd, &modifier) ||
      !eFn(dpy, view->m_dmaEglImg, fds, strides, offsets) || nfd < 1 || fds[0] < 0) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: export query failed\n");
    /* Close any fds the driver may have populated before failing.
     * fds[] was initialized to {-1,-1,-1,-1}, so we only close the
     * ones that were actually written. */
    for (int i = 0; i < 4; i++) {
      if (fds[i] >= 0)
        ::close(fds[i]);
      fds[i] = -1;
    }
    eglDestroyImage(dpy, view->m_dmaEglImg);
    view->m_dmaEglImg = EGL_NO_IMAGE_KHR;
    glBindTexture(GL_TEXTURE_2D, 0);
    return false;
  }
  view->m_dmaExportFd = fds[0];
  view->m_dmaExportFourcc = static_cast<uint32_t>(fourcc);
  view->m_dmaExportStride = static_cast<uint32_t>(strides[0]);
  view->m_dmaExportModifier = modifier;
  for (int i = 1; i < nfd && i < 4; i++)
    if (fds[i] >= 0)
      ::close(fds[i]);
  return true;
}
