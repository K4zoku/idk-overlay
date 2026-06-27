#include "webview.h"
#include "manager.h"
#include "rhi_texture_extractor.h"

#include <QPaintEvent>
#include <QApplication>
#include <QVBoxLayout>
#include <QDialog>
#include <QMenu>
#include <QDateTime>
#include <QFile>

#include <QQuickWidget>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickRenderControl>
#include <QSGRendererInterface>
#include <QWebEngineSettings>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

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
        m_buffer = 0;

        if (auto *fp = focusProxy()) fp->update();
    }

    /* ACK poll timer — single-shot, started when frameSent is emitted.
     * Polls the compositor for ACK after a frame is sent. */
    m_ackPollTimer = new QTimer(this);
    m_ackPollTimer->setSingleShot(true);
    connect(m_ackPollTimer, &QTimer::timeout, this, [this]() { pollAck(); });

    /* REQUEST poll timer — single-shot, started after ACK received.
     * Polls the compositor for a REQUEST_NEXT_FRAME message. */
    m_requestTimer = new QTimer(this);
    m_requestTimer->setSingleShot(true);
    connect(m_requestTimer, &QTimer::timeout, this, [this]() { onRequestReceived(); });

    connect(this, &WebView::frameSent, this, [this]() {
        m_ackPollTimer->start(16);
    });

    /* Overlay visibility — when hidden, stop ACK/REQUEST timers so the
     * webview's CPU usage drops to ~0%. When shown again, kick a render
     * immediately (the compositor also sends a wake-up REQUEST, but
     * calling doRenderAndSend() directly handles the first frame faster). */
    connect(m_manager, &Manager::overlayVisibleChanged,
            this, &WebView::onOverlayVisibleChanged);

    connect(this, &WebView::loadFinished, this, [this](bool ok) {
        if (!ok || m_conf.url().isEmpty()) {
            return;
        }
        /* Inject user scripts */
        QStringList scripts = m_conf.injectScripts();
        for (const QString &path : scripts) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                QString js = QString::fromUtf8(f.readAll());
                if (!js.isEmpty())
                    page()->runJavaScript(js);
            }
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

        if (!m_manager->isConnected()) {
            s_in_paint = false;
            return QWebEngineView::eventFilter(obj, event);
        }

        /* Clear resize guard — Chromium has rendered a frame at the new size */
        m_resizePending = false;

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
    if (!m_manager->isConnected())
        return;

    /* Skip frame send when overlay is hidden — the compositor would
     * drain it without ACKing, and we'd just waste a render. The
     * m_overlayVisible flag is updated via Manager::overlayVisibleChanged. */
    if (!m_overlayVisible)
        return;

    /* ACK flow control: if a frame is in flight, pollAck handles it */
    if (m_pending)
        return;

    /* After ACK with resize: skip until next Paint event clears the flag */
    if (m_resizePending)
        return;

    /* Try zero-copy DMABUF export */
    if (m_useDmaBuf && !m_dmaBufFailed) {
        if (m_extractor->tryExportDMABuf())
            return;  /* frameSent emitted → starts ackPollTimer */
    }

    /* SHM fallback */
    if (!m_memory)
        return;

    uint8_t buffer = m_buffer;
    m_buffer = (m_buffer + 1) % 2;
    uchar *shm = (uchar*)m_memory + (PIXELS_SIZE(m_renderW, m_renderH) * buffer);

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
    frame.width   = static_cast<uint32_t>(m_renderW);
    frame.height  = static_cast<uint32_t>(m_renderH);
    frame.flags   = IDK_FRAME_FLAG_VISIBLE;
    frame.nfd     = 1;

    static int s_consecutive_failures = 0;
    int rc = idk_fs_send_frame(m_memfd, &frame);
    if (rc < 0) {
        if (!idk_fs_is_connected()) {
            s_consecutive_failures = 0;
            return;
        }
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
            return;
        }
        return;  /* next Paint event will retry */
    }
    s_consecutive_failures = 0;

    IDK_LOG("webview-qt", "frame sent OK (%dx%d type=SHM fd=%d)\n",
            frame.width, frame.height, m_memfd);
    emit frameSent();

    m_pending = true;
    m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
}

void WebView::processAck(const idk_ack_msg_t &ack_msg)
{
    if (ack_msg.ack == 1) {
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

bool WebView::pollAck()
{
    if (!m_pending || !m_manager->isConnected()) {
        m_pending = false;
        m_ackPollTimer->stop();
        return false;
    }

    idk_ack_msg_t ack_msg;
    if (idk_fs_wait_ack(&ack_msg, 0) == 0) {
        m_pending = false;
        processAck(ack_msg);
        m_requestTimer->start(16);
        return true;
    }

    int now = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
    if ((now - m_sendTime) > 100) {
        m_pending = false;
        IDK_LOG("webview-qt", "ACK timeout (%dms) — force-unlock pending\n",
                now - m_sendTime);
        m_requestTimer->start(16);
        return false;
    }

    m_ackPollTimer->start(16);
    return false;
}

void WebView::onRequestReceived()
{
    if (m_pending || !m_manager->isConnected())
        return;

    idk_request_msg_t req;
    if (idk_fs_recv_request(&req, 0) == 0 && req.type == IDK_REQUEST_NEXT_FRAME) {
        m_requestTimer->stop();
        if (auto *fp = focusProxy())
            fp->update();
        return;
    }

    /* No REQUEST yet — keep polling */
    m_requestTimer->start(16);
}

void WebView::onOverlayVisibleChanged(bool visible)
{
    if (m_overlayVisible == visible) return;
    m_overlayVisible = visible;
    IDK_LOG("webview-qt", "overlay %s — %s timers\n",
            visible ? "SHOW" : "HIDE",
            visible ? "restarting" : "stopping");

    if (!visible) {
        /* Stop the ACK/REQUEST poll loop. The compositor's drain-on-hidden
         * logic (see compositor_egl.c / compositor_vk.c) will drop any
         * in-flight frame we already sent without ACKing, so m_pending
         * will be cleared by the ACK-timeout path on the next visible
         * transition. Force-clear it now so doRenderAndSend() can run
         * immediately when we become visible again. */
        m_ackPollTimer->stop();
        m_requestTimer->stop();
        m_pending = false;
    } else {
        /* Overlay visible again — kick a render immediately. The
         * compositor also sends a wake-up REQUEST on its transition
         * detection, but painting now gets the first frame on screen
         * faster than waiting for the REQUEST round-trip. */
        if (auto *fp = focusProxy())
            fp->update();
    }
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
