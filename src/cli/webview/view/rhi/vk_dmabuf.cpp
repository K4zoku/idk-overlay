#include "internal.h"
#include "rhi_texture_extractor.h"
#include "webview.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wsfinae-incomplete"
#include <QtGui/private/qrhi_p.h>
#pragma GCC diagnostic pop

#include <QDateTime>
#include <unistd.h>

#include "core/log.h"
#include "public/idk_producer.h"

#ifdef IDK_HAVE_VULKAN
/* Record the image→buffer copy with layout transitions and submit it.
 * The caller owns buffer/memory; this function cleans up its own
 * command buffer and fence. */
static bool copyImageToBuffer(VkDevice dev, VkCommandPool cmdPool, VkQueue queue, VkImage image,
                              VkImageLayout currentLayout, VkBuffer buffer, uint32_t w, uint32_t h) {
  VkCommandBufferAllocateInfo cmdAI = {};
  cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAI.commandPool = cmdPool;
  cmdAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAI.commandBufferCount = 1;

  VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(dev, &cmdAI, &cmdBuf) != VK_SUCCESS) {
    return false;
  }

  VkCommandBufferBeginInfo beginInfo = {};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  /* Check vkBeginCommandBuffer - if it fails, all subsequent vkCmd*
   * calls are no-ops or UB. */
  if (vkBeginCommandBuffer(cmdBuf, &beginInfo) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkBeginCommandBuffer failed\n");
    vkFreeCommandBuffers(dev, cmdPool, 1, &cmdBuf);
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
   * but the error path should free cmdBuf cleanly. */
  if (vkEndCommandBuffer(cmdBuf) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkEndCommandBuffer failed\n");
    vkFreeCommandBuffers(dev, cmdPool, 1, &cmdBuf);
    return false;
  }

  VkFenceCreateInfo fenceInfo = {};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  if (vkCreateFence(dev, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
    vkFreeCommandBuffers(dev, cmdPool, 1, &cmdBuf);
    return false;
  }

  VkSubmitInfo submitInfo = {};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmdBuf;

  if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkQueueSubmit failed\n");
    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, cmdPool, 1, &cmdBuf);
    return false;
  }

  /* Use a finite timeout (1 second) instead of UINT64_MAX. On timeout,
   * treat as failure and clean up. */
  VkResult waitResult = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ULL);
  vkDestroyFence(dev, fence, nullptr);
  vkFreeCommandBuffers(dev, cmdPool, 1, &cmdBuf);
  if (waitResult != VK_SUCCESS) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkWaitForFences %s (GPU hang?)\n",
            waitResult == VK_TIMEOUT ? "timed out" : "failed");
    return false;
  }
  return true;
}
#endif

bool RhiTextureExtractor::tryExportDMABufVulkan() {
#ifdef IDK_HAVE_VULKAN
  if (!m_view->m_vk.resolved) {
    QQuickWindow *window = qobject_cast<QQuickWidget *>(m_view->focusProxy())->quickWindow();
    auto *rif = window->rendererInterface();
    m_view->initVulkan(rif, window);
    if (!m_view->m_vk.resolved)
      return false;
  }

  bool rhiRtMissing = false;
  QRhiTexture *tex = getRhiColorTexture(m_view, &rhiRtMissing);
  if (rhiRtMissing) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: RhiRedirectRenderTarget null\n");
    return false;
  }
  if (!tex)
    return false;

  QRhiTexture::NativeTexture native = tex->nativeTexture();
  VkImage image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(native.object));
  VkImageLayout currentLayout = static_cast<VkImageLayout>(native.layout);
  if (!image) {
    IDK_LOG("webview-qt", "tryExportDMABufVulkan: native VkImage is null\n");
    return false;
  }

  VkDevice dev = m_view->m_vk.device;
  QSize texSize = tex->pixelSize();
  uint32_t w = static_cast<uint32_t>(texSize.width());
  uint32_t h = static_cast<uint32_t>(texSize.height());
  VkDeviceSize bufSize = w * h * 4;

  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (!createDmaBufBuffer(dev, m_view->m_vk.physDev, bufSize, &buffer, &memory))
    return false;

  if (!copyImageToBuffer(dev, m_view->m_vk.cmdPool, m_view->m_vk.queue, image, currentLayout, buffer, w, h)) {
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  int dmabufFd = -1;
  if (!exportMemoryFd(dev, memory, m_view->m_vkGetMemoryFdKHR, &dmabufFd)) {
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    return false;
  }

  bool sent = sendDmaBufFrame(m_view, w, h, w * 4, 0x34324241, 0, 0, dmabufFd);
  ::close(dmabufFd);
  vkDestroyBuffer(dev, buffer, nullptr);
  vkFreeMemory(dev, memory, nullptr);
  return sent;
#endif
  return false;
}
