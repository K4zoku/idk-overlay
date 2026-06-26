#include "webview.h"
#include "manager.h"
#include "rhi_texture_extractor.h"

#include <QPaintEvent>
#include <QApplication>
#include <QVBoxLayout>
#include <QDialog>
#include <QMenu>
#include <QDateTime>

#include <QQuickWidget>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickRenderControl>
#include <QSGRendererInterface>
#include <QWebEngineSettings>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wsfinae-incomplete"
#include <QtGui/private/qrhi_p.h>  // Private header for RHI access
#pragma GCC diagnostic pop

#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>

#include "public/idk_fs.h"
#include "core/log.h"



#define PIXELS_SIZE(w, h)  ((w) * (h) * 4)

WebView::WebView(uint8_t id, const GroupConfig &conf, Manager *manager, bool noDmaBuf, QWidget *parent)
    : QWebEngineView(parent)
    , m_id(id)
    , m_conf(conf)
    , m_manager(manager)
{
    m_extractor = new RhiTextureExtractor(this);
    if (noDmaBuf) m_useDmaBuf = false;
    setPage(new WebPage);
    settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    m_renderW = m_conf.width();
    m_renderH = m_conf.height();

    page()->setBackgroundColor(Qt::transparent);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMinimumSize(m_conf.width(), m_conf.height());
    setMaximumSize(m_conf.width(), m_conf.height());
    load(m_conf.url());

    // Use DMA-BUF if available, otherwise SHM
    if (!m_manager->isConnected()) {
        // Wait for socket connection before sending
        connect(m_manager, &Manager::socketConnected, this, [this]() {
            /* Guard against double-init on reconnect — skip if already init'd */
            if (m_memory) {
                IDK_LOG("webview-qt", "Overlay %u reconnected (memory already init'd, skipping re-init)\n", m_id);

                if (auto *fp = focusProxy()) {
                    fp->update();
                }

                QTimer::singleShot(100, this, [this]() {
                    if (auto *fp = focusProxy()) fp->update();
                });
                return;
            }
            initDmaBuf();
            initMemory();
            focusProxy()->installEventFilter(this);

            sendCreateImage();
            m_waitReply = false;  /* ready to send first frame */
            m_buffer = 0;
            /* Force first paint */
            if (auto *fp = focusProxy()) fp->update();
        });
    } else {
        // Already connected, initialize immediately
        initDmaBuf();
        initMemory();
        focusProxy()->installEventFilter(this);

        sendCreateImage();
        m_waitReply = false;  /* ready to send first frame */
        m_buffer = 0;

        if (auto *fp = focusProxy()) fp->update();
    }

    /* Heartbeat timer — keeps ACK polling alive when Chromium
       stops generating paint events (e.g. after resize). */
    m_heartbeat = new QTimer(this);
    m_heartbeat->setTimerType(Qt::CoarseTimer);
    connect(m_heartbeat, &QTimer::timeout, this, [this]() { doRenderAndSend(); });

    connect(this, &WebView::loadFinished, this, [this](bool ok) {
        if (!ok || m_conf.url().isEmpty()) {
            return;
        }
        /* Force paint after load */
        if (auto *fp = focusProxy()) fp->update();

    });
}

WebView::~WebView()
{
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

bool WebView::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == focusProxy() && event->type() == QEvent::Paint) {
        /* Guard recursive paint */
        static bool s_in_paint = false;
        if (s_in_paint) {
            return QWebEngineView::eventFilter(obj, event);
        }
        s_in_paint = true;

        if (m_waitReply || !m_manager->isConnected()) {
            s_in_paint = false;
            return QWebEngineView::eventFilter(obj, event);
        }

        /* Clear resize guard — Chromium has rendered a frame at the new size */
        m_resizePending = false;

        /* Start heartbeat to keep polling even if Chromium stops
           generating paint events (e.g. after a resize). */
        if (m_heartbeat && !m_heartbeat->isActive())
            m_heartbeat->start(50);

        QTimer::singleShot(0, this, [this]() {
            doRenderAndSend();
        });

        s_in_paint = false;
        return true;
    }
    return QWebEngineView::eventFilter(obj, event);
}

void WebView::doRenderAndSend()
{
    if (m_waitReply || !m_manager->isConnected()) {
        QTimer::singleShot(16, this, [this]() { doRenderAndSend(); });
        return;
    }

    /* ACK flow control: non-blocking poll + 100ms safety timeout */
    if (m_pending) {
        int ack_fd = idk_fs_get_fd();
        if (ack_fd >= 0) {
            struct pollfd pfd = { .fd = ack_fd, .events = POLLIN, .revents = 0 };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                struct {
                    uint8_t ack;
                    int32_t w;
                    int32_t h;
                } ack_msg;
                memset(&ack_msg, 0, sizeof(ack_msg));
                if (read(ack_fd, &ack_msg, sizeof(ack_msg)) > 0) {
                    m_pending = false;
                    if (ack_msg.ack == 1) {
                        /* Compositor rejected this frame's DMABUF. Could be
                         * transient (e.g. first frame after resize when Qt
                         * RHI's texture isn't fully rebuilt yet, or NVIDIA
                         * driver hiccup on vkGetMemoryFdPropertiesKHR).
                         * Only fall back to SHM permanently after N
                         * CONSECUTIVE failures — a single success resets
                         * the counter. */
                        m_dmabufRejectCount++;
                        if (m_dmabufRejectCount >= 5 && !m_dmaBufFailed) {
                            IDK_LOG("webview-qt", "compositor rejected DMABUF %d times — falling back to SHM\n",
                                    m_dmabufRejectCount);
                            m_dmaBufFailed = true;
                        } else if (!m_dmaBufFailed) {
                            IDK_LOG("webview-qt", "compositor rejected DMABUF (%d/5) — will retry\n",
                                    m_dmabufRejectCount);
                        }
                    } else {
                        /* ack=0: frame accepted. Reset the consecutive
                         * failure counter — DMABUF is working. */
                        if (m_dmabufRejectCount > 0) {
                            IDK_LOG("webview-qt", "DMABUF accepted after %d rejection(s) — counter reset\n",
                                    m_dmabufRejectCount);
                            m_dmabufRejectCount = 0;
                        }
                    }
                    if (ack_msg.w > 0 && ack_msg.h > 0) {
                        IDK_LOG("webview-qt", "ACK received with game size: %dx%d\n",
                                ack_msg.w, ack_msg.h);
                        resizeForGame(ack_msg.w, ack_msg.h);
                        m_resizePending = true;
                    }
                }
            }
        }
        int now = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
        if (m_pending && (now - m_sendTime) > 100) {
            IDK_LOG("webview-qt", "ACK timeout (%dms) — force-unlock pending\n",
                    now - m_sendTime);
            m_pending = false;
        }
        if (m_pending) {
            QTimer::singleShot(4, this, [this]() { doRenderAndSend(); });
            return;
        }
    }

    /* After ACK with resize: skip frame send until Chromium
       produces a new frame at the new size (next Paint event). */
    if (m_resizePending) {
        /* heartbeat keeps polling; paint event clears the flag */
        return;
    }

    /* ── Try zero-copy DMABUF export (backend auto-detected) ──
     * Skip DMABUF during the post-resize cooldown window: Qt RHI is
     * rebuilding its render-target texture in this period and exporting
     * DMABUF mid-rebuild can SIGSEGV (the QRhiTexture pointer we
     * obtained may be invalidated underneath us). Force SHM instead. */
    qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (now_ms < m_dmabufCooldownUntil) {
        /* Log cooldown start once per cooldown period */
        static qint64 s_last_cooldown_log = 0;
        if (s_last_cooldown_log != m_dmabufCooldownUntil) {
            IDK_LOG("webview-qt", "DMABUF cooldown active (%lldms remaining) — forcing SHM\n",
                    m_dmabufCooldownUntil - now_ms);
            s_last_cooldown_log = m_dmabufCooldownUntil;
        }
    } else if (m_useDmaBuf && !m_dmaBufFailed) {
        if (m_extractor->tryExportDMABuf())
            return;
    }

    /* ── SHM fallback: glReadPixels directly into SHM (0 CPU copies) ── */
    if (!m_memory) {
        QTimer::singleShot(16, this, [this]() { doRenderAndSend(); });
        return;
    }

    uint8_t buffer = m_buffer;
    m_buffer = (m_buffer + 1) % 2;
    uchar *shm = (uchar*)m_memory + (PIXELS_SIZE(m_renderW, m_renderH) * buffer);

    bool read_ok = m_extractor->tryReadPixelsToSHM(shm, m_renderW, m_renderH);

    if (!read_ok) {
        /* Fallback: grabFramebuffer → row-reverse memcpy (2 CPU copies) */
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
    }

    idk_fs_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.width   = static_cast<uint32_t>(m_renderW);
    frame.height  = static_cast<uint32_t>(m_renderH);
    frame.flags   = IDK_FRAME_FLAG_VISIBLE;  /* SHM: no DMABUF bit */
    frame.nfd     = 1;
    /* stride=0 for SHM, modifier=0 */

    static int s_consecutive_failures = 0;

    /* SHM frame — use idk_fs_send_frame (NOT send_dma_buf) so the
     * DMABUF flag is NOT set. Using send_dma_buf here was a bug:
     * it unconditionally sets IDK_FRAME_FLAG_DMABUF, causing the
     * compositor to try EGL/GL_EXT_memory_object import on a memfd
     * (which is not a dmabuf) → import fails every frame → black screen. */
    int rc = idk_fs_send_frame(m_memfd, &frame);
    if (rc < 0) {
        s_consecutive_failures++;
        if (s_consecutive_failures <= 3 || s_consecutive_failures % 60 == 0) {
            qWarning("[idk-webview] send failed (attempt %d): %s",
                     s_consecutive_failures, strerror(errno));
        }
        if (s_consecutive_failures > 300) {
            IDK_LOG("webview-qt", "Too many consecutive send failures — "
                    "forcing disconnect\n");
            idk_fs_shutdown();
            s_consecutive_failures = 0;
        }
        QTimer::singleShot(16, this, [this]() { doRenderAndSend(); });
        return;
    }
    s_consecutive_failures = 0;

    IDK_LOG("webview-qt", "frame sent OK (%dx%d type=SHM fd=%d)\n",
            frame.width, frame.height, m_memfd);
    emit frameSent();

    m_pending = true;
    m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;

    /* Keep heartbeat alive while waiting for ACK */
    if (m_heartbeat && !m_heartbeat->isActive())
        m_heartbeat->start(50);
}

void WebView::resizeForGame(int w, int h)
{
    if (w == m_renderW && h == m_renderH) return;
    if (m_resizing) {
        IDK_LOG("webview-qt", "game resize: %dx%d (re-entered, skipped)\n", w, h);
        return;
    }
    m_resizing = true;

    IDK_LOG("webview-qt", "game resize: %dx%d -> %dx%d\n",
            m_renderW, m_renderH, w, h);

    /* Arm DMABUF cooldown: Qt RHI rebuilds its render-target texture during
     * and after the widget resize; exporting DMABUF mid-rebuild can crash.
     * 300ms is enough for Qt to settle at any frame rate. */
    m_dmabufCooldownUntil = QDateTime::currentMSecsSinceEpoch() + 300;

    setMinimumSize(w, h);
    setMaximumSize(w, h);
    resize(w, h);

    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    /* Toggle the RenderWidgetHostViewQtDelegateItem (QQuickItem) visibility
       to trigger notifyHidden() → notifyShown() on RenderWidgetHostViewQt.
       This is the actual mechanism that:
       - Evicts the old frame and detaches the DelegatedFrameHost (notifyHidden)
       - Re-attaches the DelegatedFrameHost and requests a new frame from the
         renderer at the current viewport size (notifyShown)
       page()->setVisible() only toggles WebContents visibility which does NOT
       trigger the delegate item's ItemVisibleHasChanged event, so it doesn't
       actually re-attach the DelegatedFrameHost. */
    if (auto *qw = qobject_cast<QQuickWidget *>(focusProxy())) {
        if (QQuickItem *root = qw->rootObject()) {
            for (auto *child : root->childItems()) {
                child->setVisible(false);
                child->setVisible(true);
            }
        }
    }

    /* Reuse existing SHM if current allocation is large enough */
    size_t needed = PIXELS_SIZE(w, h) * 2;
    if (m_memsize >= needed) {
        m_renderW = w;
        m_renderH = h;
        m_buffer = 0;
        m_pending = false;
        m_dmaBufFailed = false;
        m_dmabufRejectCount = 0;  /* reset on resize — allow DMABUF retry */
        if (m_heartbeat) m_heartbeat->start(50);
        if (auto *fp = focusProxy()) fp->update();
        m_resizing = false;
        return;
    }

    if (m_memory) {
        munmap(m_memory, m_memsize);
        m_memory = nullptr;
    }
    if (m_memfd >= 0) {
        ::close(m_memfd);
        m_memfd = -1;
    }

    m_renderW = w;
    m_renderH = h;
    m_buffer = 0;

    initMemory();
    m_pending = false;
    m_dmaBufFailed = false;
    m_dmabufRejectCount = 0;

    /* Restart heartbeat to poll ACKs/resume frame delivery after resize */
    if (m_heartbeat) m_heartbeat->start(50);

    if (auto *fp = focusProxy()) fp->update();

    m_resizing = false;
}

void WebView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu;
    menu.addAction(pageAction(QWebEnginePage::Back));
    menu.addAction(pageAction(QWebEnginePage::Forward));
    menu.addAction(pageAction(QWebEnginePage::Reload));
    menu.addSeparator();
    menu.addAction(pageAction(QWebEnginePage::ViewSource));
    menu.addAction(tr("Inspect"), this, [=, this]() {
        if (page()->devToolsPage()) {
            triggerPageAction(QWebEnginePage::InspectElement);
        } else {
            QWebEngineView *view = createWindow(QWebEnginePage::WebDialog);
            view->page()->setInspectedPage(page());
        }
    });
    menu.exec(event->globalPos());
}

QWebEngineView *WebView::createWindow(QWebEnginePage::WebWindowType type)
{
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

void WebView::initMemory()
{
    m_memsize = PIXELS_SIZE(m_renderW, m_renderH) * 2;

    m_memfd = memfd_create("idk-webview", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (m_memfd < 0) {
        perror("memfd_create");
        return;
    }

    if (ftruncate(m_memfd, m_memsize) < 0) {
        perror("ftruncate");
        ::close(m_memfd);
        m_memfd = -1;
        return;
    }

    m_memory = mmap(NULL, m_memsize, PROT_READ | PROT_WRITE, MAP_SHARED, m_memfd, 0);
    if (m_memory == MAP_FAILED) {
        perror("mmap");
        ::close(m_memfd);
        m_memfd = -1;
        return;
    }

    fcntl(m_memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
    fcntl(m_memfd, F_ADD_SEALS, F_SEAL_SEAL);
}

void WebView::initDmaBuf()
{
    m_eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_eglDpy == EGL_NO_DISPLAY) {
        IDK_LOG("webview-qt", "initDmaBuf: eglGetDisplay failed\n");
        m_useDmaBuf = false;
        return;
    }

    EGLint major, minor;
    if (!eglInitialize(m_eglDpy, &major, &minor)) {
        IDK_LOG("webview-qt", "initDmaBuf: eglInitialize failed\n");
        m_eglDpy = EGL_NO_DISPLAY;
        m_useDmaBuf = false;
        return;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        IDK_LOG("webview-qt", "initDmaBuf: eglBindAPI failed\n");
        eglTerminate(m_eglDpy);
        m_eglDpy = EGL_NO_DISPLAY;
        m_useDmaBuf = false;
        return;
    }

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint ncfg;
    if (!eglChooseConfig(m_eglDpy, config_attribs, &config, 1, &ncfg) || ncfg == 0) {
        IDK_LOG("webview-qt", "initDmaBuf: eglChooseConfig failed\n");
        eglTerminate(m_eglDpy);
        m_eglDpy = EGL_NO_DISPLAY;
        m_useDmaBuf = false;
        return;
    }
    m_eglConfig = config;

    static const EGLint pbuf_attribs[] = {
        EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE
    };
    m_eglSurf = eglCreatePbufferSurface(m_eglDpy, config, pbuf_attribs);
    if (m_eglSurf == EGL_NO_SURFACE) {
        IDK_LOG("webview-qt", "initDmaBuf: eglCreatePbufferSurface failed\n");
        eglTerminate(m_eglDpy);
        m_eglDpy = EGL_NO_DISPLAY;
        m_useDmaBuf = false;
        return;
    }

    /* Create our EGL context. Start unshared; will share with Qt lazily
       when its OpenGL context becomes available. */
    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    m_eglCtx = eglCreateContext(m_eglDpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (m_eglCtx == EGL_NO_CONTEXT) {
        IDK_LOG("webview-qt", "initDmaBuf: eglCreateContext failed\n");
        eglDestroySurface(m_eglDpy, m_eglSurf);
        m_eglSurf = EGL_NO_SURFACE;
        eglTerminate(m_eglDpy);
        m_eglDpy = EGL_NO_DISPLAY;
        m_useDmaBuf = false;
        return;
    }
    m_needSharedCtx = true;

    /* Opportunistically share with Qt's context now (may not be ready yet) */
    if (m_extractor) m_extractor->ensureDmaBufSharedCtx();

    /* Resolve DMABUF export entry points — try MESA then EXT */
    m_queryFn  = eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    m_exportFn = eglGetProcAddress("eglExportDMABUFImageMESA");
    if (!m_queryFn || !m_exportFn) {
        m_queryFn  = eglGetProcAddress("eglExportDMABUFImageQueryEXT");
        m_exportFn = eglGetProcAddress("eglExportDMABUFImageEXT");
    }
    m_dmabufResolved = (m_queryFn && m_exportFn);

    if (!m_needSharedCtx && m_dmabufResolved)
        IDK_LOG("webview-qt", "Zero-copy DMABUF ready (shared ctx)\n");
    else
        IDK_LOG("webview-qt", "DMABUF deferred (%s%s)\n",
                m_needSharedCtx ? "no Qt ctx yet" : "",
                !m_dmabufResolved ? "no export funcs" : "");
}

void WebView::initVulkan(QSGRendererInterface *rif, QQuickWindow *window)
{
    (void)rif; (void)window;
#ifdef IDK_HAVE_VULKAN
    if (m_vk.resolved) return;

    m_vk.instance = static_cast<VkInstance>(
        rif->getResource(window, QSGRendererInterface::VulkanInstanceResource));
    m_vk.physDev = static_cast<VkPhysicalDevice>(
        rif->getResource(window, QSGRendererInterface::PhysicalDeviceResource));
    m_vk.device = static_cast<VkDevice>(
        rif->getResource(window, QSGRendererInterface::DeviceResource));
    m_vk.queue = static_cast<VkQueue>(
        rif->getResource(window, QSGRendererInterface::CommandQueueResource));

    if (auto *qf = static_cast<uint32_t *>(
            rif->getResource(window, QSGRendererInterface::GraphicsQueueFamilyIndexResource)))
        m_vk.queueFamily = *qf;

    if (!m_vk.device || !m_vk.physDev || !m_vk.queue) {
        IDK_LOG("webview-qt", "Vulkan init: device/physDev/queue not available\n");
        return;
    }

    // Resolve vkGetMemoryFdKHR via device extension
    m_vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(m_vk.device, "vkGetMemoryFdKHR"));
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

void WebView::sendCreateImage()
{
    IDK_LOG("webview-qt", "Overlay %u ready: %dx%d@(%d,%d)\n",
            m_id, m_conf.width(), m_conf.height(), m_conf.x(), m_conf.y());
}

void WebPage::javaScriptConsoleMessage(QWebEnginePage::JavaScriptConsoleMessageLevel level,
                                       const QString &message,
                                       int lineNumber,
                                       const QString &sourceId)
{
    Q_UNUSED(level);
    Q_UNUSED(sourceId);
    if (message.startsWith(QLatin1String("idk:")))
        qDebug() << "[idk-js]" << message << "at line" << lineNumber;
}
