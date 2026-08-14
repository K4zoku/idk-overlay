#include "internal.h"
#include "webview.h"

#include <QDateTime>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "core/log.h"
#include "public/idk_producer.h"

static gbm_device *s_gbm = nullptr;

void restoreEglCurrent(EGLDisplay dpy, EGLContext ctx, EGLSurface read, EGLSurface draw) {
  if (dpy != EGL_NO_DISPLAY)
    eglMakeCurrent(dpy, draw, read, ctx);
}

gbm_bo *createGbmStagingBo(int w, int h, int *outFd, uint32_t *outStride, uint64_t *outModifier) {
  if (!s_gbm) {
    int rfd = open("/dev/dri/renderD128", O_RDWR);
    if (rfd < 0)
      rfd = open("/dev/dri/renderD129", O_RDWR);
    if (rfd >= 0)
      s_gbm = gbm_create_device(rfd);
  }
  if (!s_gbm)
    return nullptr;

  gbm_bo *bo = gbm_bo_create(s_gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING);
  if (!bo) {
    IDK_LOG("webview-qt", "gbm_bo_create(RENDERING) returned NULL\n");
    return nullptr;
  }
  *outFd = gbm_bo_get_fd(bo);
  *outStride = gbm_bo_get_stride(bo);
  *outModifier = gbm_bo_get_modifier(bo);
  IDK_LOG("webview-qt", "gbm bo: fd=%d stride=%u mod=0x%llx\n", *outFd, *outStride, (unsigned long long)*outModifier);
  return bo;
}

EGLImageKHR createStagingEglImage(EGLDisplay dpy, int w, int h, int fd, uint32_t stride) {
  EGLAttrib attrs[] = {EGL_WIDTH,
                       w,
                       EGL_HEIGHT,
                       h,
                       EGL_LINUX_DRM_FOURCC_EXT,
                       GBM_FORMAT_ABGR8888,
                       EGL_DMA_BUF_PLANE0_FD_EXT,
                       fd,
                       EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                       0,
                       EGL_DMA_BUF_PLANE0_PITCH_EXT,
                       (EGLAttrib)stride,
                       EGL_NONE};
  return eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
}

static bool bindDmaBufTexture(GLenum target, EGLImageKHR image, void *fnTexStorage, void *fnTex2DOES) {
  bool bound = false;
  if (fnTexStorage) {
    ((void (*)(GLenum, void *, const GLint *))fnTexStorage)(target, (void *)image, NULL);
    bound = (glGetError() == GL_NO_ERROR);
  }
  if (!bound && fnTex2DOES) {
    ((void (*)(GLenum, void *))fnTex2DOES)(target, (void *)image);
    bound = (glGetError() == GL_NO_ERROR);
  }
  return bound;
}

bool setupStagingDmabuf(WebView *view, EGLDisplay dpy, int w, int h, void *fnTexStorage, void *fnTex2DOES) {
  /* Recreate copy textures on size/ownership change. Single GPU blit:
   *   staging dmabuf (tiled, gbm RENDERING) ← Qt texture
   * so the exported fd matches the tiled layout the overlay's
   * GL_EXT_memory_object import creates (i915 doesn't convert layout).
   * m_dmaTex is only created as a fallback when gbm fails. No CPU/RAM
   * copy anywhere. */
  if (view->m_dmaTex != 0 && view->m_dmaTexW == w && view->m_dmaTexH == h)
    return true;

  if (view->m_dmaEglImgLinear != EGL_NO_IMAGE_KHR) {
    eglDestroyImage(dpy, view->m_dmaEglImgLinear);
    view->m_dmaEglImgLinear = EGL_NO_IMAGE_KHR;
  }
  if (view->m_dmaExportFd >= 0) {
    ::close(view->m_dmaExportFd);
    view->m_dmaExportFd = -1;
  }
  view->m_dmaExportFourcc = 0;
  view->m_dmaExportStride = 0;
  view->m_dmaExportModifier = 0;

  if (view->m_dmaTexLinear)
    glDeleteTextures(1, &view->m_dmaTexLinear);
  view->m_dmaTexLinear = 0;
  if (view->m_dmaTex)
    glDeleteTextures(1, &view->m_dmaTex);
  view->m_dmaTex = 0;

  int linearFd = -1;
  uint32_t linearStride = 0;
  uint64_t realMod = 0;
  gbm_bo *bo = createGbmStagingBo(w, h, &linearFd, &linearStride, &realMod);
  if (bo)
    gbm_bo_destroy(bo);
  if (linearFd >= 0 && linearStride > 0) {
    view->m_dmaEglImgLinear = createStagingEglImage(dpy, w, h, linearFd, linearStride);
    if (view->m_dmaEglImgLinear != EGL_NO_IMAGE_KHR) {
      glGenTextures(1, &view->m_dmaTexLinear);
      glBindTexture(GL_TEXTURE_2D, view->m_dmaTexLinear);
      bool bound = bindDmaBufTexture(GL_TEXTURE_2D, view->m_dmaEglImgLinear, fnTexStorage, fnTex2DOES);
      if (bound) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        view->m_dmaExportFd = linearFd;
        view->m_dmaExportFourcc = GBM_FORMAT_ABGR8888;
        view->m_dmaExportStride = linearStride;
        view->m_dmaExportModifier = realMod;
        IDK_LOG("webview-qt", "STAGING dmabuf: fd=%d stride=%u fourcc=0x%x mod=0x%llx\n", linearFd, linearStride,
                (unsigned)GBM_FORMAT_ABGR8888, (unsigned long long)realMod);
      } else {
        glDeleteTextures(1, &view->m_dmaTexLinear);
        view->m_dmaTexLinear = 0;
        eglDestroyImage(dpy, view->m_dmaEglImgLinear);
        view->m_dmaEglImgLinear = EGL_NO_IMAGE_KHR;
      }
    }
    if (view->m_dmaTexLinear == 0)
      ::close(linearFd);
  }

  /* Fallback copy texture (tiled, export via eglExportDMABUFImageMESA)
   * — only used when the gbm staging path failed above. */
  if (view->m_dmaTexLinear == 0) {
    glGenTextures(1, &view->m_dmaTex);
    glBindTexture(GL_TEXTURE_2D, view->m_dmaTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  view->m_dmaTexW = w;
  view->m_dmaTexH = h;
  view->m_dmaBufId++;
  if (view->m_dmaBufId == 0)
    view->m_dmaBufId = 1;
  return true;
}

bool sendDmaBufFrame(WebView *view, int w, int h, uint32_t stride, uint32_t fourcc, uint64_t modifier, uint16_t bufId,
                     int fd) {
  idk_frame_header_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.width = static_cast<uint32_t>(w);
  frame.height = static_cast<uint32_t>(h);
  frame.flags = IDK_FRAME_FLAG_VISIBLE;
  frame.nfd = 1;
  frame.stride = stride;
  frame.fourcc = fourcc;
  frame.modifier = modifier;
  frame.buf_id = bufId;

  int fds[4] = {fd, -1, -1, -1};
  int rc = idk_producer_send_dma_buf(fds, &frame);

  if (rc == 0) {
    view->m_buffer = (view->m_buffer + 1) % 2;
    view->m_pending = true;
    view->m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
    emit view->frameSent();
    return true;
  }

  static int s_dmabuf_send_fail = 0;
  s_dmabuf_send_fail++;
  if (s_dmabuf_send_fail == 1 || s_dmabuf_send_fail % 60 == 0) {
    IDK_LOG("webview-qt", "idk_producer_send_dma_buf failed rc=%d (attempt %d, errno=%d: %s)\n", rc, s_dmabuf_send_fail,
            errno, strerror(errno));
  }
  if (s_dmabuf_send_fail > 5 && !idk_producer_is_connected()) {
    s_dmabuf_send_fail = 0;
  }
  return false;
}
