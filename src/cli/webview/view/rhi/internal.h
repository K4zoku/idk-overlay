#ifndef IDK_WEBVIEW_RHI_INTERNAL_H
#define IDK_WEBVIEW_RHI_INTERNAL_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <stdint.h>

struct WebView;
class QRhiTexture;

/* gbm.cpp — restore the EGL context/surfaces that were current before an
 * export (no-op when no display was current). */
void restoreEglCurrent(EGLDisplay dpy, EGLContext ctx, EGLSurface read, EGLSurface draw);

/* readpixels.cpp — resolve the Qt RHI redirect render target's color
 * texture. Returns nullptr on failure. *outRhiRtMissing is set when the
 * RhiRedirectRenderTarget resource is absent (the GL export path treats
 * that case differently from the SHM path). */
QRhiTexture *getRhiColorTexture(WebView *view, bool *outRhiRtMissing);

/* readpixels.cpp — export the copy texture as dmabuf via
 * eglExportDMABUFImageMESA/EXT (used by gl_dmabuf.cpp). */
bool exportDmaBufTexture(WebView *view, EGLDisplay dpy, EGLContext ctx);

/* gbm.cpp — tiled staging buffer helpers. The driver-default tiled layout
 * matches the overlay's GL_EXT_memory_object import (i915 doesn't convert
 * layout, so a LINEAR buffer would be read as tiled). */
gbm_bo *createGbmStagingBo(int w, int h, int *outFd, uint32_t *outStride, uint64_t *outModifier);
EGLImageKHR createStagingEglImage(EGLDisplay dpy, int w, int h, int fd, uint32_t stride);

/* gbm.cpp — (re)create the staging dmabuf / fallback copy textures for w×h.
 * fnTexStorage / fnTex2DOES are the resolved glEGLImageTarget* pointers. */
bool setupStagingDmabuf(WebView *view, EGLDisplay dpy, int w, int h, void *fnTexStorage, void *fnTex2DOES);

/* gbm.cpp — send a dmabuf frame over the transport; shared by the GL and
 * Vulkan export paths. Returns true on success. */
bool sendDmaBufFrame(WebView *view, int w, int h, uint32_t stride, uint32_t fourcc, uint64_t modifier, uint16_t bufId,
                     int fd);

#ifdef IDK_HAVE_VULKAN
#include <vulkan/vulkan.h>
/* gl_dmabuf.cpp — Vulkan staging-buffer creation (used by vk_dmabuf.cpp). */
bool createDmaBufBuffer(VkDevice dev, VkPhysicalDevice physDev, VkDeviceSize size, VkBuffer *outBuffer,
                        VkDeviceMemory *outMemory);
/* readpixels.cpp — export a Vulkan staging buffer's memory as a dma-buf fd. */
bool exportMemoryFd(VkDevice dev, VkDeviceMemory memory, PFN_vkGetMemoryFdKHR fn, int *outFd);
#endif

#endif /* IDK_WEBVIEW_RHI_INTERNAL_H */
