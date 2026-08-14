#include "internal.h"
#include "rhi_texture_extractor.h"
#include "webview.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wsfinae-incomplete"
#include <QtGui/private/qrhi_p.h>
#pragma GCC diagnostic pop

#include "core/log.h"

/* FBO blit: Qt texture → staging dmabuf texture (GPU). Fallback: Qt
 * texture → tiled copy texture (exported via eglExportDMABUF). */
static bool blitToExportTexture(WebView *view, GLuint texId, int w, int h, void *fnGenFramebuffers,
                                void *fnBindFramebuffer, void *fnFramebufferTexture2D, void *fnBlitFramebuffer,
                                void *fnDeleteFramebuffers) {
  if (!fnGenFramebuffers || !fnBindFramebuffer || !fnFramebufferTexture2D || !fnBlitFramebuffer ||
      !fnDeleteFramebuffers) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: FBO functions not available\n");
    return false;
  }

  typedef void (*PFN_glGenFramebuffers)(GLsizei, GLuint *);
  typedef void (*PFN_glBindFramebuffer)(GLenum, GLuint);
  typedef void (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  typedef void (*PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
  typedef void (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);

  GLuint readFbo = 0, drawFbo = 0;
  ((PFN_glGenFramebuffers)fnGenFramebuffers)(1, &readFbo);
  ((PFN_glGenFramebuffers)fnGenFramebuffers)(1, &drawFbo);
  ((PFN_glBindFramebuffer)fnBindFramebuffer)(0x8CA8, readFbo);
  ((PFN_glFramebufferTexture2D)fnFramebufferTexture2D)(0x8CA8, 0x8CE0, 0x0DE1, texId, 0);
  ((PFN_glBindFramebuffer)fnBindFramebuffer)(0x8CA9, drawFbo);
  ((PFN_glFramebufferTexture2D)fnFramebufferTexture2D)(
      0x8CA9, 0x8CE0, 0x0DE1, view->m_dmaTexLinear != 0 ? view->m_dmaTexLinear : view->m_dmaTex, 0);
  ((PFN_glBlitFramebuffer)fnBlitFramebuffer)(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2600);
  ((PFN_glBindFramebuffer)fnBindFramebuffer)(0x8D40, 0);
  ((PFN_glDeleteFramebuffers)fnDeleteFramebuffers)(1, &readFbo);
  ((PFN_glDeleteFramebuffers)fnDeleteFramebuffers)(1, &drawFbo);
  return true;
}

#ifdef IDK_HAVE_VULKAN
/* Vulkan staging-buffer creation for the vk_dmabuf.cpp export path. */
bool createDmaBufBuffer(VkDevice dev, VkPhysicalDevice physDev, VkDeviceSize bufSize, VkBuffer *outBuffer,
                        VkDeviceMemory *outMemory) {
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
  vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

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
   * operates on an unbound buffer → UB. */
  if (vkBindBufferMemory(dev, buffer, memory, 0) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkBindBufferMemory failed\n");
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }
  *outBuffer = buffer;
  *outMemory = memory;
  return true;
}
#endif

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
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
    return false;
  }

  EGLDisplay exportDpy = m_view->m_eglDpy;
  EGLContext exportCtx = m_view->m_eglCtx;

  bool rhiRtMissing = false;
  QRhiTexture *tex = getRhiColorTexture(m_view, &rhiRtMissing);
  if (rhiRtMissing) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: RhiRedirectRenderTarget null\n");
    m_view->m_dmaBufFailed = true;
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
    return false;
  }
  if (!tex) {
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
    return false;
  }
  QRhiTexture::NativeTexture native = tex->nativeTexture();
  GLuint texId = static_cast<GLuint>(native.object);
  if (!texId) {
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
    return false;
  }
  int w = tex->pixelSize().width();
  int h = tex->pixelSize().height();

  resolveImageBindGL();
  setupStagingDmabuf(m_view, exportDpy, w, h, s_fn_glEGLImageTargetTexStorageEXT, s_fn_glEGLImageTargetTexture2DOES);

  resolveFBOGL();
  if (!blitToExportTexture(m_view, texId, w, h, s_fn_glGenFramebuffers, s_fn_glBindFramebuffer,
                           s_fn_glFramebufferTexture2D, s_fn_glBlitFramebuffer, s_fn_glDeleteFramebuffers)) {
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
    return false;
  }

  /* Fence sync after blit */
  if (!fenceSyncGL()) {
    IDK_LOG("webview-qt", "tryExportDMABufOpenGL: blit fence sync failed\n");
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
    return false;
  }

  if (!exportDmaBufTexture(m_view, exportDpy, exportCtx)) {
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
    return false;
  }

  if (!sendDmaBufFrame(m_view, w, h, m_view->m_dmaExportStride, m_view->m_dmaExportFourcc, m_view->m_dmaExportModifier,
                       m_view->m_dmaBufId, m_view->m_dmaExportFd)) {
    glBindTexture(GL_TEXTURE_2D, 0);
    restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
    return false;
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  restoreEglCurrent(savedDpy, savedCtx, savedRead, savedDraw);
  return true;
}
