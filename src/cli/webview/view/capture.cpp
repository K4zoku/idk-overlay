#include "rhi_texture_extractor.h"
#include "webview.h"

#include <QDateTime>
#include <QDialog>
#include <QPaintEvent>
#include <QQuickWidget>
#include <QSGRendererInterface>
#include <QVBoxLayout>

#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "core/log.h"
#include "public/idk_producer.h"

#define PIXELS_SIZE(w, h) ((w) * (h) * 4)

bool WebView::eventFilter(QObject *obj, QEvent *event) {
  if (obj == focusProxy() && event->type() == QEvent::Paint) {
    static bool s_in_paint = false;
    if (s_in_paint)
      return QWebEngineView::eventFilter(obj, event);
    s_in_paint = true;

    if (!m_manager->isConnected()) {
      s_in_paint = false;
      return QWebEngineView::eventFilter(obj, event);
    }

    m_resizePending = false;

    QTimer::singleShot(0, this, [this]() { doRenderAndSend(); });

    s_in_paint = false;
    return true;
  }
  return QWebEngineView::eventFilter(obj, event);
}

void WebView::doRenderAndSend() {
  if (!m_manager->isConnected())
    return;

  if (!m_overlayVisible)
    return;

  if (m_pending)
    return;

  if (m_resizePending)
    return;

  if (m_useDmaBuf && !m_dmaBufFailed) {
    if (m_extractor->tryExportDMABuf())
      return;
  }

  if (!m_memory)
    return;

  uint8_t buffer = m_buffer;
  m_buffer = (m_buffer + 1) % 2;
  uchar *shm = (uchar *)m_memory + (PIXELS_SIZE(m_renderW, m_renderH) * buffer);

  bool read_ok = m_extractor->tryReadPixelsToSHM(shm, m_renderW, m_renderH);

  if (!read_ok) {
    if (auto *qw = qobject_cast<QQuickWidget *>(focusProxy())) {
      QImage img = qw->grabFramebuffer();
      int bpr = qMin(img.bytesPerLine(), m_renderW * 4);
      for (int y = 0; y < m_renderH; y++) {
        const uchar *srcRow = img.constScanLine(m_renderH - 1 - y);
        uchar *dstRow = shm + y * (m_renderW * 4);
        memcpy(dstRow, srcRow, bpr);
      }
      m_framePremultiplied = true;
      read_ok = true;
    }
    if (!read_ok)
      return;
  }

  idk_frame_header_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.width = static_cast<uint32_t>(m_renderW);
  frame.height = static_cast<uint32_t>(m_renderH);
  frame.flags = IDK_FRAME_FLAG_VISIBLE;
  frame.nfd = 1;

  static int s_consecutive_failures = 0;
  int rc = idk_producer_send_frame(m_memfd, &frame);
  if (rc < 0) {
    if (!idk_producer_is_connected()) {
      s_consecutive_failures = 0;
      return;
    }
    s_consecutive_failures++;
    if (s_consecutive_failures <= 3 || s_consecutive_failures % 60 == 0) {
      qWarning("[idk-webview] send failed (attempt %d): %s", s_consecutive_failures, strerror(errno));
    }
    if (s_consecutive_failures > 300) {
      IDK_LOG("webview-qt", "Too many consecutive send failures - "
                            "forcing disconnect\n");
      idk_producer_shutdown();
      s_consecutive_failures = 0;
      return;
    }
    return;
  }
  s_consecutive_failures = 0;

  IDK_LOG("webview-qt", "frame sent OK (%dx%d type=SHM fd=%d)\n", frame.width, frame.height, m_memfd);
  emit frameSent();

  m_pending = true;
  m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
}

void WebView::initVulkan(QSGRendererInterface *rif, QQuickWindow *window) {
  (void)rif;
  (void)window;
#ifdef IDK_HAVE_VULKAN
  if (m_vk.resolved)
    return;

  m_vk.instance = static_cast<VkInstance>(rif->getResource(window, QSGRendererInterface::VulkanInstanceResource));
  m_vk.physDev = static_cast<VkPhysicalDevice>(rif->getResource(window, QSGRendererInterface::PhysicalDeviceResource));
  m_vk.device = static_cast<VkDevice>(rif->getResource(window, QSGRendererInterface::DeviceResource));
  m_vk.queue = static_cast<VkQueue>(rif->getResource(window, QSGRendererInterface::CommandQueueResource));

  if (auto *qf =
          static_cast<uint32_t *>(rif->getResource(window, QSGRendererInterface::GraphicsQueueFamilyIndexResource)))
    m_vk.queueFamily = *qf;

  if (!m_vk.device || !m_vk.physDev || !m_vk.queue) {
    IDK_LOG("webview-qt", "Vulkan init: device/physDev/queue not available\n");
    return;
  }

  m_vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(m_vk.device, "vkGetMemoryFdKHR"));
  if (!m_vkGetMemoryFdKHR) {
    IDK_LOG("webview-qt", "Vulkan init: vkGetMemoryFdKHR not found\n");
    return;
  }

  VkCommandPoolCreateInfo poolInfo = {};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolInfo.queueFamilyIndex = m_vk.queueFamily;
  if (vkCreateCommandPool(m_vk.device, &poolInfo, nullptr, &m_vk.cmdPool) != VK_SUCCESS) {
    IDK_LOG("webview-qt", "Vulkan init: vkCreateCommandPool failed\n");
    return;
  }

  m_vk.resolved = true;
  IDK_LOG("webview-qt", "Vulkan DMABUF ready (dev=%p)\n", (void *)m_vk.device);
#endif
}

void WebView::sendCreateImage() {
  IDK_LOG("webview-qt", "Overlay %u ready: %dx%d@\n", m_id, m_conf.width(), m_conf.height());
}

QWebEngineView *WebView::createWindow(QWebEnginePage::WebWindowType type) {
  (void)type;
  QWebEngineView *view = new QWebEngineView;
  QVBoxLayout *layout = new QVBoxLayout;
  layout->addWidget(view);
  QDialog *dialog = new QDialog(parentWidget());
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setLayout(layout);
  dialog->resize(parentWidget()->width() * 0.8, parentWidget()->height() * 0.8);
  dialog->show();
  return view;
}

WebView::~WebView() {
  delete m_extractor;
  if (m_eglCtx != EGL_NO_CONTEXT) {
    eglDestroyContext(m_eglDpy, m_eglCtx);
  }
  if (m_eglSurf != EGL_NO_SURFACE) {
    eglDestroySurface(m_eglDpy, m_eglSurf);
  }
  if (m_eglDpy != EGL_NO_DISPLAY) {
    eglTerminate(m_eglDpy);
  }

  if (m_memory) {
    munmap(m_memory, m_memsize);
  }
  if (m_memfd >= 0) {
    ::close(m_memfd);
  }
}
