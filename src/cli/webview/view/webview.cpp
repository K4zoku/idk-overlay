#include "webview.h"
#include "manager.h"

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

#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <dlfcn.h>

#include "public/idk_fs.h"
#include "core/log.h"





#define PIXELS_SIZE(w, h)  ((w) * (h) * 4)

WebView::WebView(uint8_t id, const GroupConfig &conf, Manager *manager, bool noDmaBuf, QWidget *parent)
    : QWebEngineView(parent)
    , m_id(id)
    , m_conf(conf)
    , m_manager(manager)
{
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
        if (tryExportDMABuf())
            return;     // sent zero-copy, pending
        /* tryExportDMABuf() sets m_dmaBufFailed for permanent failures only;
           transient failures (e.g. Qt context not ready) retry next cycle. */
    }

    /* ── SHM fallback: glReadPixels directly into SHM (0 CPU copies) ── */
    if (!m_memory) {
        QTimer::singleShot(16, this, [this]() { doRenderAndSend(); });
        return;
    }

    uint8_t buffer = m_buffer;
    m_buffer = (m_buffer + 1) % 2;
    uchar *shm = (uchar*)m_memory + (PIXELS_SIZE(m_renderW, m_renderH) * buffer);

    /* Try 0-copy glReadPixels path first. Falls back to grabFramebuffer
     * if shared context not available (e.g. EGL init failed). */
    bool read_ok = tryReadPixelsToSHM(shm, m_renderW, m_renderH);

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
    ensureDmaBufSharedCtx();

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

/* ── Lazily create EGL context shared with Qt's Quick window OpenGL context ── */
bool WebView::ensureDmaBufSharedCtx()
{
    if (!m_needSharedCtx)
        return true;

    EGLContext qtEglCtx = EGL_NO_CONTEXT;
    EGLDisplay qtEglDpy = EGL_NO_DISPLAY;
    EGLConfig qtEglConfig = nullptr;

    if (auto *qw = qobject_cast<QQuickWidget *>(focusProxy())) {
        if (auto *window = qw->quickWindow()) {
            auto *rif = window->rendererInterface();
            if (rif) {
                auto *qtCtx = static_cast<QOpenGLContext *>(
                    rif->getResource(window, QSGRendererInterface::OpenGLContextResource));
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

    /* Query Qt's EGL config — we MUST use the same config to share contexts.
     * EGL_BAD_MATCH (0x3006) happens when our config doesn't match Qt's. */
    EGLint qtConfigId = 0;
    if (qtEglDpy != EGL_NO_DISPLAY) {
        eglQueryContext(qtEglDpy, qtEglCtx, EGL_CONFIG_ID, &qtConfigId);
        if (qtConfigId > 0) {
            EGLint cfg_attribs[] = { EGL_CONFIG_ID, qtConfigId, EGL_NONE };
            EGLint ncfg = 0;
            eglChooseConfig(qtEglDpy, cfg_attribs, &qtEglConfig, 1, &ncfg);
            if (ncfg > 0 && qtEglConfig) {
                /* Use Qt's display and config instead of ours */
                m_eglDpy = qtEglDpy;
                m_eglConfig = qtEglConfig;
                /* Recreate pbuffer surface with Qt's config */
                if (m_eglSurf != EGL_NO_SURFACE) {
                    eglDestroySurface(m_eglDpy, m_eglSurf);
                    m_eglSurf = EGL_NO_SURFACE;
                }
                static const EGLint pbuf_attribs[] = {
                    EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE
                };
                m_eglSurf = eglCreatePbufferSurface(m_eglDpy, m_eglConfig, pbuf_attribs);
            }
        }
    }

    /* Destroy old unshared context, create one shared with Qt */
    if (m_eglCtx != EGL_NO_CONTEXT)
        eglDestroyContext(m_eglDpy, m_eglCtx);

    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    m_eglCtx = eglCreateContext(m_eglDpy, m_eglConfig, qtEglCtx, ctx_attribs);
    if (m_eglCtx == EGL_NO_CONTEXT) {
        EGLint err = eglGetError();
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            IDK_LOG("webview-qt", "ensureDmaBufSharedCtx: eglCreateContext failed (eglError=0x%x) — "
                    "DMABUF disabled, using SHM fallback\n", err);
        }
        m_useDmaBuf = false;
        return false;
    }

    m_needSharedCtx = false;
    IDK_LOG("webview-qt", "DMABUF context now shared with Qt (config_id=%d)\n", qtConfigId);
    return true;
}

// ── DMABUF dispatcher — detects RHI backend and dispatches ──
bool WebView::tryExportDMABuf()
{
    QQuickWidget *qw = qobject_cast<QQuickWidget *>(focusProxy());
    if (!qw) return false;
    QQuickWindow *window = qw->quickWindow();
    if (!window) return false;
    auto *rif = window->rendererInterface();
    if (!rif) return false;

    auto api = rif->graphicsApi();
    switch (api) {
    case QSGRendererInterface::OpenGL:
        return tryExportDMABufOpenGL();
#ifdef IDK_HAVE_VULKAN
    case QSGRendererInterface::Vulkan:
        return tryExportDMABufVulkan();
#endif
    default:
        m_dmaBufFailed = true;
        return false;
    }
}

// ── OpenGL DMABUF export (zero-copy: sync via fence + export Qt's texture) ──
bool WebView::tryExportDMABufOpenGL()
{
    /* Lazy shared-context init — Qt's GL context may not be ready at init time */
    if (m_needSharedCtx && !ensureDmaBufSharedCtx())
        return false;

    QQuickWidget *qw = qobject_cast<QQuickWidget *>(focusProxy());
    if (!qw) return false;
    QQuickWindow *window = qw->quickWindow();
    if (!window) return false;

    auto *rif = window->rendererInterface();
    if (!rif) return false;

    /* ── Step 1: Get Qt's GL context (for logging only) ──
     * QQuickWidget has already rendered to the texture (on paint event /
     * updateTimer). We just need to sync the GPU before exporting. */
    QOpenGLContext *qtCtx = static_cast<QOpenGLContext *>(
        rif->getResource(window, QSGRendererInterface::OpenGLContextResource));
    if (!qtCtx) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: Qt GL context not found\n");
        return false;
    }

    /* Save & restore the caller's EGL context */
    EGLDisplay savedDpy = eglGetCurrentDisplay();
    EGLContext savedCtx = eglGetCurrentContext();
    EGLSurface savedRead = eglGetCurrentSurface(EGL_READ);
    EGLSurface savedDraw = eglGetCurrentSurface(EGL_DRAW);

    /* Make OUR shared context current — NOT Qt's context.
     *
     * Qt's QQuickWindow is an offscreen window, so qtCtx->makeCurrent(window)
     * fails. But our shared EGL context shares texture IDs with Qt's context,
     * and Mesa's iris/i965 driver uses a shared command buffer for contexts
     * in the same share group. So fence sync on our context flushes ALL
     * pending commands, including Qt's render commands. */
    if (!eglMakeCurrent(m_eglDpy, m_eglSurf, m_eglSurf, m_eglCtx)) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: eglMakeCurrent failed for shared ctx\n");
        return false;
    }

    /* ── Step 2: GPU sync via fence sync ──
     *
     * glFinish() alone didn't work (texture still white). The issue is
     * that Mesa's command queue for offscreen render targets may not
     * fully flush with glFinish alone. Using a fence sync with
     * GL_SYNC_FLUSH_COMMANDS_BIT forces the driver to flush all pending
     * commands before waiting, ensuring the texture write is visible.
     *
     * This is the same mechanism Mesa uses internally for
     * endOffscreenFrame() with readback (which grabFramebuffer uses). */
    /* Resolve fence sync functions via dlsym (not in our gl_loader) */
    typedef void* (*PFN_glFenceSync)(unsigned int, unsigned int);
    typedef unsigned int (*PFN_glClientWaitSync)(void*, unsigned int, uint64_t);
    typedef void (*PFN_glDeleteSync)(void*);
    static PFN_glFenceSync fn_glFenceSync = nullptr;
    static PFN_glClientWaitSync fn_glClientWaitSync = nullptr;
    static PFN_glDeleteSync fn_glDeleteSync = nullptr;
    if (!fn_glFenceSync) {
        void *lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libOpenGL.so.0", RTLD_NOW);
        if (lib) {
            fn_glFenceSync = (PFN_glFenceSync)dlsym(lib, "glFenceSync");
            fn_glClientWaitSync = (PFN_glClientWaitSync)dlsym(lib, "glClientWaitSync");
            fn_glDeleteSync = (PFN_glDeleteSync)dlsym(lib, "glDeleteSync");
        }
    }

    /* GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117
     * GL_SYNC_FLUSH_COMMANDS_BIT = 0x00000001
     * GL_ALREADY_SIGNALED = 0x911A, GL_CONDITION_SATISFIED = 0x911C */
    if (fn_glFenceSync && fn_glClientWaitSync && fn_glDeleteSync) {
        void *fence = fn_glFenceSync(0x9117, 0);
        if (fence) {
            glFlush();
            unsigned int result = fn_glClientWaitSync(fence, 0x00000001, 1000000000ULL); /* 1s timeout */
            fn_glDeleteSync(fence);
            if (result != 0x911A && result != 0x911C) {
                IDK_LOG("webview-qt", "tryExportDMABufOpenGL: fence sync failed (result=0x%x)\n", result);
                if (savedDpy != EGL_NO_DISPLAY)
                    eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
                return false;
            }
        } else {
            glFinish();
        }
    } else {
        /* Fallback to glFinish if fence sync not available */
        glFinish();
    }

    /* Use our shared EGL context for eglCreateImage — texture IDs are
     * shared between our context and Qt's, so we can reference Qt's
     * texture from our context. */
    EGLDisplay exportDpy = m_eglDpy;
    EGLContext exportCtx = m_eglCtx;

    /* ── Step 3: Get the render target's color attachment texture ──
     * Read AFTER GPU sync — Qt RHI may have swapped textures during render. */
    auto *rhiRt = reinterpret_cast<QRhiTextureRenderTarget *>(
        rif->getResource(window, QSGRendererInterface::RhiRedirectRenderTarget));
    if (!rhiRt) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: RhiRedirectRenderTarget null\n");
        m_dmaBufFailed = true;
        if (savedDpy != EGL_NO_DISPLAY)
            eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
        return false;
    }

    auto desc = rhiRt->description();
    if (desc.colorAttachmentCount() == 0) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: no color attachment\n");
        if (savedDpy != EGL_NO_DISPLAY)
            eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
        return false;
    }
    const QRhiColorAttachment *ca = desc.colorAttachmentAt(0);
    if (!ca || !ca->texture()) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: no texture in attachment\n");
        if (savedDpy != EGL_NO_DISPLAY)
            eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
        return false;
    }

    QRhiTexture::NativeTexture native = ca->texture()->nativeTexture();
    GLuint texId = static_cast<GLuint>(native.object);
    if (!texId) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: null texture ID\n");
        if (savedDpy != EGL_NO_DISPLAY)
            eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
        return false;
    }

    int w = ca->texture()->pixelSize().width();
    int h = ca->texture()->pixelSize().height();
    if (w <= 0 || h <= 0) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: invalid texture size %dx%d\n", w, h);
        if (savedDpy != EGL_NO_DISPLAY)
            eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
        return false;
    }

    /* Log texture ID changes for debugging */
    static GLuint s_last_tex = 0;
    static int s_frame_count = 0;
    s_frame_count++;
    if (texId != s_last_tex || s_frame_count % 60 == 0) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: texId=%u (was=%u frame=%d size=%dx%d)\n",
                texId, s_last_tex, s_frame_count, w, h);
        s_last_tex = texId;
    }

    bool ok = false;

    /* ── Step 4: Copy Qt's render target texture to our own regular texture ──
     *
     * Qt's texture is created with QRhiTexture::RenderTarget flag.
     * eglCreateImage(EGL_GL_TEXTURE_2D) on render target textures doesn't
     * preserve content on Mesa — the dmabuf is created but points to
     * empty/uninitialized memory (appears as WHITE or transparent).
     *
     * glCopyImageSubData also doesn't work reliably for render target
     * textures — content may be transparent even though copy succeeds.
     *
     * Fix: use FBO blit (glBlitFramebuffer) — the reliable way to read
     * from a render target texture. Attach Qt's texture to a read FBO,
     * attach our texture to a draw FBO, blit. This is 1 GPU copy.
     *
     * Tradeoff: still 1 GPU copy (vs 0 for true zero-copy which doesn't
     * work on Mesa). But no CPU involvement, no PCIe bandwidth. */
    if (m_dmaTex == 0 || m_dmaTexW != w || m_dmaTexH != h) {
        /* Texture changed — invalidate cached EGLImage + exported fd.
         * They must be recreated for the new texture. */
        if (m_dmaEglImg != EGL_NO_IMAGE_KHR) {
            eglDestroyImage(exportDpy, m_dmaEglImg);
            m_dmaEglImg = EGL_NO_IMAGE_KHR;
        }
        if (m_dmaExportFd >= 0) { ::close(m_dmaExportFd); m_dmaExportFd = -1; }
        m_dmaExportFourcc = 0;
        m_dmaExportStride = 0;
        m_dmaExportModifier = 0;

        if (m_dmaTex) glDeleteTextures(1, &m_dmaTex);
        glGenTextures(1, &m_dmaTex);
        glBindTexture(GL_TEXTURE_2D, m_dmaTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_dmaTexW = w;
        m_dmaTexH = h;
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: created copy texture %dx%d tex=%u\n",
                w, h, m_dmaTex);
    }

    /* Resolve FBO functions via dlsym (not in our gl_loader) */
    typedef void (*PFN_glGenFramebuffers)(GLsizei, GLuint*);
    typedef void (*PFN_glBindFramebuffer)(GLenum, GLuint);
    typedef void (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    typedef void (*PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    typedef void (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
    static PFN_glGenFramebuffers fn_glGenFramebuffers = nullptr;
    static PFN_glBindFramebuffer fn_glBindFramebuffer = nullptr;
    static PFN_glFramebufferTexture2D fn_glFramebufferTexture2D = nullptr;
    static PFN_glBlitFramebuffer fn_glBlitFramebuffer = nullptr;
    static PFN_glDeleteFramebuffers fn_glDeleteFramebuffers = nullptr;
    if (!fn_glGenFramebuffers) {
        void *lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libOpenGL.so.0", RTLD_NOW);
        if (lib) {
            fn_glGenFramebuffers = (PFN_glGenFramebuffers)dlsym(lib, "glGenFramebuffers");
            fn_glBindFramebuffer = (PFN_glBindFramebuffer)dlsym(lib, "glBindFramebuffer");
            fn_glFramebufferTexture2D = (PFN_glFramebufferTexture2D)dlsym(lib, "glFramebufferTexture2D");
            fn_glBlitFramebuffer = (PFN_glBlitFramebuffer)dlsym(lib, "glBlitFramebuffer");
            fn_glDeleteFramebuffers = (PFN_glDeleteFramebuffers)dlsym(lib, "glDeleteFramebuffers");
        }
    }

    /* GL constants:
     * GL_READ_FRAMEBUFFER = 0x8CA8
     * GL_DRAW_FRAMEBUFFER = 0x8CA9
     * GL_FRAMEBUFFER = 0x8D40
     * GL_COLOR_ATTACHMENT0 = 0x8CE0
     * GL_FRAMEBUFFER_COMPLETE = 0x8CD5
     * GL_COLOR_BUFFER_BIT = 0x4000
     * GL_NEAREST = 0x2600
     * GL_TEXTURE_2D = 0x0DE1 */
    if (fn_glGenFramebuffers && fn_glBindFramebuffer && fn_glFramebufferTexture2D &&
        fn_glBlitFramebuffer && fn_glDeleteFramebuffers) {
        GLuint readFbo = 0, drawFbo = 0;
        fn_glGenFramebuffers(1, &readFbo);
        fn_glGenFramebuffers(1, &drawFbo);

        /* Attach Qt's texture (source) to read FBO */
        fn_glBindFramebuffer(0x8CA8, readFbo);
        fn_glFramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, texId, 0);

        /* Attach our texture (dest) to draw FBO */
        fn_glBindFramebuffer(0x8CA9, drawFbo);
        fn_glFramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, m_dmaTex, 0);

        /* Blit: source is Y-up (framebuffer origin = bottom-left),
         * dest is also Y-up. No flip needed — both use framebuffer coords.
         * glBlitFramebuffer(0,0,w,h, 0,0,w,h, COLOR_BUFFER_BIT, NEAREST) */
        fn_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2600);

        /* Unbind FBOs */
        fn_glBindFramebuffer(0x8D40, 0);
        fn_glDeleteFramebuffers(1, &readFbo);
        fn_glDeleteFramebuffers(1, &drawFbo);
    } else {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: FBO functions not available, falling back to SHM\n");
        if (savedDpy != EGL_NO_DISPLAY)
            eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
        return false;
    }

    /* Fence sync again to ensure the blit is complete before export */
    if (fn_glFenceSync && fn_glClientWaitSync && fn_glDeleteSync) {
        void *fence2 = fn_glFenceSync(0x9117, 0);
        if (fence2) {
            glFlush();
            unsigned int result2 = fn_glClientWaitSync(fence2, 0x00000001, 1000000000ULL);
            fn_glDeleteSync(fence2);
            if (result2 != 0x911A && result2 != 0x911C) {
                IDK_LOG("webview-qt", "tryExportDMABufOpenGL: blit fence sync failed (result=0x%x)\n", result2);
                if (savedDpy != EGL_NO_DISPLAY)
                    eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
                return false;
            }
        }
    }

    /* ── Step 5: Export OUR texture as dmabuf ──
     * Our texture is a regular GL texture (not render target), so
     * eglCreateImage works correctly on Mesa.
     *
     * REUSE: eglCreateImage + eglExportDMABUFImageMESA are expensive on Mesa
     * (each call hits the kernel to allocate/export a dma-buf). We cache the
     * EGLImage and the exported fd across frames as long as m_dmaTex + size
     * don't change. The fd is dup'd per send — sendmsg takes ownership of
     * the dup, not the original fd. This reduces per-frame export cost to a
     * single dup() + sendmsg(). */
    glBindTexture(GL_TEXTURE_2D, m_dmaTex);

    /* (Re)create EGLImage + export fd if texture changed or first time. */
    if (m_dmaEglImg == EGL_NO_IMAGE_KHR) {
        m_dmaEglImg = eglCreateImage(
            exportDpy, exportCtx, EGL_GL_TEXTURE_2D,
            reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(m_dmaTex)),
            nullptr);
        if (!m_dmaEglImg) {
            IDK_LOG("webview-qt", "tryExportDMABufOpenGL: eglCreateImage failed (0x%x)\n", eglGetError());
            glBindTexture(GL_TEXTURE_2D, 0);
            if (savedDpy != EGL_NO_DISPLAY)
                eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
            return false;
        }
        /* Query + export fd ONCE — reuse for all subsequent frames. */
        EGLint fourcc = 0, nfd = 0;
        EGLuint64KHR modifier = 0;
        int fds[4] = {-1, -1, -1, -1};
        EGLint strides[4] = {0};
        EGLint offsets[4] = {0};

        auto qFn = reinterpret_cast<EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLuint64KHR *)>(m_queryFn);
        auto eFn = reinterpret_cast<EGLBoolean (EGLAPIENTRY *)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLint *)>(m_exportFn);

        if (!qFn || !eFn ||
            !qFn(exportDpy, m_dmaEglImg, &fourcc, &nfd, &modifier) ||
            !eFn(exportDpy, m_dmaEglImg, fds, strides, offsets) ||
            nfd < 1 || fds[0] < 0) {
            IDK_LOG("webview-qt", "tryExportDMABufOpenGL: export query failed\n");
            eglDestroyImage(exportDpy, m_dmaEglImg);
            m_dmaEglImg = EGL_NO_IMAGE_KHR;
            glBindTexture(GL_TEXTURE_2D, 0);
            if (savedDpy != EGL_NO_DISPLAY)
                eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
            return false;
        }
        m_dmaExportFd = fds[0];
        m_dmaExportFourcc = static_cast<uint32_t>(fourcc);
        m_dmaExportStride = static_cast<uint32_t>(strides[0]);
        m_dmaExportModifier = modifier;
        /* Close any extra plane fds (single-plane BGRA8888 only uses fds[0]). */
        for (int i = 1; i < nfd && i < 4; i++)
            if (fds[i] >= 0) ::close(fds[i]);
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: export OK (fourcc=0x%x modifier=0x%llx stride=%u fd=%d) — cached for reuse\n",
                fourcc, (unsigned long long)modifier, m_dmaExportStride, m_dmaExportFd);
    }

    /* dup the fd — sendmsg takes ownership of the dup, the original stays open
     * in m_dmaExportFd for the next frame. */
    int sendFd = dup(m_dmaExportFd);
    if (sendFd < 0) {
        IDK_LOG("webview-qt", "tryExportDMABufOpenGL: dup(fd=%d) failed: %s\n",
                m_dmaExportFd, strerror(errno));
        glBindTexture(GL_TEXTURE_2D, 0);
        if (savedDpy != EGL_NO_DISPLAY)
            eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
        return false;
    }

    {
        idk_fs_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.width   = static_cast<uint32_t>(w);
        frame.height  = static_cast<uint32_t>(h);
        frame.flags   = IDK_FRAME_FLAG_VISIBLE;  /* DMABUF bit set by idk_fs_send_dma_buf */
        frame.nfd     = 1;
        frame.stride  = m_dmaExportStride;
        frame.fourcc  = m_dmaExportFourcc;
        frame.modifier = m_dmaExportModifier;

        int fds_arr[4] = { sendFd, -1, -1, -1 };
        int rc = idk_fs_send_dma_buf(fds_arr, &frame);
        if (sendFd >= 0) ::close(sendFd);  /* sendmsg took its own ref via dup */

        if (rc == 0) {
            IDK_LOG("webview-qt", "frame sent OK (%dx%d type=DMABUF fd=%d stride=%u)\n",
                    w, h, m_dmaExportFd, frame.stride);
            m_buffer = (m_buffer + 1) % 2;
            m_pending = true;
            m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
            emit frameSent();
            ok = true;
        } else {
            IDK_LOG("webview-qt", "tryExportDMABufOpenGL: idk_fs_send_dma_buf failed rc=%d\n", rc);
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    if (savedDpy != EGL_NO_DISPLAY)
        eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);

    return ok;
}

// ── SHM 0-copy path: glReadPixels directly into SHM buffer ──
bool WebView::tryReadPixelsToSHM(uchar *shm, int w, int h)
{
    /* Lazy shared-context init — same as DMABUF path.
     * Even when --no-dmabuf is set, we need the shared EGL context
     * to access Qt's render target texture. */
    if (m_needSharedCtx && !ensureDmaBufSharedCtx())
        return false;

    QQuickWidget *qw = qobject_cast<QQuickWidget *>(focusProxy());
    if (!qw) return false;
    QQuickWindow *window = qw->quickWindow();
    if (!window) return false;

    auto *rif = window->rendererInterface();
    if (!rif) return false;

    /* Get Qt's render target texture */
    auto *rhiRt = reinterpret_cast<QRhiTextureRenderTarget *>(
        rif->getResource(window, QSGRendererInterface::RhiRedirectRenderTarget));
    if (!rhiRt) return false;

    auto desc = rhiRt->description();
    if (desc.colorAttachmentCount() == 0) return false;
    const QRhiColorAttachment *ca = desc.colorAttachmentAt(0);
    if (!ca || !ca->texture()) return false;

    QRhiTexture::NativeTexture native = ca->texture()->nativeTexture();
    GLuint texId = static_cast<GLuint>(native.object);
    if (!texId) return false;

    /* Save & restore the caller's EGL context */
    EGLDisplay savedDpy = eglGetCurrentDisplay();
    EGLContext savedCtx = eglGetCurrentContext();
    EGLSurface savedRead = eglGetCurrentSurface(EGL_READ);
    EGLSurface savedDraw = eglGetCurrentSurface(EGL_DRAW);

    /* Make our shared context current */
    if (!eglMakeCurrent(m_eglDpy, m_eglSurf, m_eglSurf, m_eglCtx)) {
        IDK_LOG("webview-qt", "tryReadPixelsToSHM: eglMakeCurrent failed\n");
        return false;
    }

    /* Fence sync — flush Qt's render commands via shared command buffer
     * (same pattern as tryExportDMABufOpenGL) */
    typedef void* (*PFN_glFenceSync)(unsigned int, unsigned int);
    typedef unsigned int (*PFN_glClientWaitSync)(void*, unsigned int, uint64_t);
    typedef void (*PFN_glDeleteSync)(void*);
    static PFN_glFenceSync fn_glFenceSync = nullptr;
    static PFN_glClientWaitSync fn_glClientWaitSync = nullptr;
    static PFN_glDeleteSync fn_glDeleteSync = nullptr;
    if (!fn_glFenceSync) {
        void *lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libOpenGL.so.0", RTLD_NOW);
        if (lib) {
            fn_glFenceSync = (PFN_glFenceSync)dlsym(lib, "glFenceSync");
            fn_glClientWaitSync = (PFN_glClientWaitSync)dlsym(lib, "glClientWaitSync");
            fn_glDeleteSync = (PFN_glDeleteSync)dlsym(lib, "glDeleteSync");
        }
    }
    if (fn_glFenceSync && fn_glClientWaitSync && fn_glDeleteSync) {
        void *fence = fn_glFenceSync(0x9117, 0);
        if (fence) {
            glFlush();
            unsigned int result = fn_glClientWaitSync(fence, 0x00000001, 1000000000ULL);
            fn_glDeleteSync(fence);
            if (result != 0x911A && result != 0x911C) {
                IDK_LOG("webview-qt", "tryReadPixelsToSHM: fence sync failed (0x%x)\n", result);
                if (savedDpy != EGL_NO_DISPLAY)
                    eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
                return false;
            }
        }
    } else {
        glFinish();
    }

    /* Resolve FBO + glReadPixels functions via dlsym */
    typedef void (*PFN_glGenFramebuffers)(GLsizei, GLuint*);
    typedef void (*PFN_glBindFramebuffer)(GLenum, GLuint);
    typedef void (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    typedef void (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
    typedef void (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
    static PFN_glGenFramebuffers fn_glGenFramebuffers = nullptr;
    static PFN_glBindFramebuffer fn_glBindFramebuffer = nullptr;
    static PFN_glFramebufferTexture2D fn_glFramebufferTexture2D = nullptr;
    static PFN_glReadPixels fn_glReadPixels = nullptr;
    static PFN_glDeleteFramebuffers fn_glDeleteFramebuffers = nullptr;
    if (!fn_glGenFramebuffers) {
        void *lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libOpenGL.so.0", RTLD_NOW);
        if (lib) {
            fn_glGenFramebuffers = (PFN_glGenFramebuffers)dlsym(lib, "glGenFramebuffers");
            fn_glBindFramebuffer = (PFN_glBindFramebuffer)dlsym(lib, "glBindFramebuffer");
            fn_glFramebufferTexture2D = (PFN_glFramebufferTexture2D)dlsym(lib, "glFramebufferTexture2D");
            fn_glReadPixels = (PFN_glReadPixels)dlsym(lib, "glReadPixels");
            fn_glDeleteFramebuffers = (PFN_glDeleteFramebuffers)dlsym(lib, "glDeleteFramebuffers");
        }
    }
    if (!fn_glGenFramebuffers || !fn_glBindFramebuffer || !fn_glFramebufferTexture2D ||
        !fn_glReadPixels || !fn_glDeleteFramebuffers) {
        if (savedDpy != EGL_NO_DISPLAY)
            eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
        return false;
    }

    /* Attach Qt's texture to read FBO */
    GLuint readFbo = 0;
    fn_glGenFramebuffers(1, &readFbo);
    fn_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, readFbo);
    fn_glFramebufferTexture2D(0x8CA8, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */,
                              0x0DE1 /* GL_TEXTURE_2D */, texId, 0);

    /* Set pack alignment for tightly packed RGBA (4 bytes per pixel) */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    /* glReadPixels reads bottom-up (row 0 = bottom of framebuffer).
     * glTexImage2D in compositor expects bottom-up data.
     * → NO flip, NO row-reverse memcpy needed — orientation matches! */
    fn_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, shm);

    /* Cleanup */
    fn_glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, 0);
    fn_glDeleteFramebuffers(1, &readFbo);

    if (savedDpy != EGL_NO_DISPLAY)
        eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);

    m_framePremultiplied = true;  /* Qt RHI textures are premultiplied */
    return true;
}

// ── Vulkan DMABUF export — staged buffer copy ──
bool WebView::tryExportDMABufVulkan()
{
#ifdef IDK_HAVE_VULKAN
    if (!m_vk.resolved) {
        QQuickWindow *window = qobject_cast<QQuickWidget *>(focusProxy())->quickWindow();
        auto *rif = window->rendererInterface();
        initVulkan(rif, window);
        if (!m_vk.resolved) return false;
    }

    QQuickWidget *qw = qobject_cast<QQuickWidget *>(focusProxy());
    if (!qw) return false;
    QQuickWindow *window = qw->quickWindow();
    if (!window) return false;
    auto *rif = window->rendererInterface();
    if (!rif) return false;

    auto *rhiRt = reinterpret_cast<QRhiTextureRenderTarget *>(
        rif->getResource(window, QSGRendererInterface::RhiRedirectRenderTarget));
    if (!rhiRt) {
        IDK_LOG("webview-qt", "tryExportDMABufVulkan: RhiRedirectRenderTarget null\n");
        return false;
    }

    auto desc = rhiRt->description();
    if (desc.colorAttachmentCount() == 0) return false;
    const QRhiColorAttachment *ca = desc.colorAttachmentAt(0);
    if (!ca || !ca->texture()) return false;

    QRhiTexture::NativeTexture native = ca->texture()->nativeTexture();
    VkImage image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(native.object));
    VkImageLayout currentLayout = static_cast<VkImageLayout>(native.layout);
    if (!image) {
        IDK_LOG("webview-qt", "tryExportDMABufVulkan: native VkImage is null\n");
        return false;
    }

    VkDevice dev = m_vk.device;
    QSize texSize = ca->texture()->pixelSize();
    uint32_t w = static_cast<uint32_t>(texSize.width());
    uint32_t h = static_cast<uint32_t>(texSize.height());
    VkDeviceSize bufSize = w * h * 4;

    // Create staging buffer with exportable DMABUF memory
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
    vkGetPhysicalDeviceMemoryProperties(m_vk.physDev, &memProps);

    uint32_t memTypeIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (!(memReqs.memoryTypeBits & (1u << i))) continue;
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
        IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkAllocateMemory failed (size=%llu)\n",
                (unsigned long long)bufSize);
        vkDestroyBuffer(dev, buffer, nullptr);
        return false;
    }
    vkBindBufferMemory(dev, buffer, memory, 0);

    // One-shot command buffer
    VkCommandBufferAllocateInfo cmdAI = {};
    cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAI.commandPool = m_vk.cmdPool;
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
    vkBeginCommandBuffer(cmdBuf, &beginInfo);

    // Transition image: current → TRANSFER_SRC_OPTIMAL
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

    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    // Copy VkImage → VkBuffer
    VkBufferImageCopy copyRegion = {};
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageExtent.width = w;
    copyRegion.imageExtent.height = h;
    copyRegion.imageExtent.depth = 1;

    vkCmdCopyImageToBuffer(cmdBuf, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copyRegion);

    // Transition back to original layout
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

    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &back);

    vkEndCommandBuffer(cmdBuf);

    // Submit and wait
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(dev, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(dev, m_vk.cmdPool, 1, &cmdBuf);
        vkDestroyBuffer(dev, buffer, nullptr);
        vkFreeMemory(dev, memory, nullptr);
        return false;
    }

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    if (vkQueueSubmit(m_vk.queue, 1, &submitInfo, fence) != VK_SUCCESS) {
        IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkQueueSubmit failed\n");
        vkDestroyFence(dev, fence, nullptr);
        vkFreeCommandBuffers(dev, m_vk.cmdPool, 1, &cmdBuf);
        vkDestroyBuffer(dev, buffer, nullptr);
        vkFreeMemory(dev, memory, nullptr);
        return false;
    }

    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, m_vk.cmdPool, 1, &cmdBuf);

    // Export DMABUF fd
    if (!m_vkGetMemoryFdKHR) {
        vkDestroyBuffer(dev, buffer, nullptr);
        vkFreeMemory(dev, memory, nullptr);
        return false;
    }

    VkMemoryGetFdInfoKHR fdInfo = {};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = memory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    int dmabufFd = -1;
    if (m_vkGetMemoryFdKHR(dev, &fdInfo, &dmabufFd) != VK_SUCCESS || dmabufFd < 0) {
        IDK_LOG("webview-qt", "tryExportDMABufVulkan: vkGetMemoryFdKHR failed\n");
        vkDestroyBuffer(dev, buffer, nullptr);
        vkFreeMemory(dev, memory, nullptr);
        return false;
    }

    // Send via existing frame protocol
    idk_fs_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.width   = w;
    frame.height  = h;
    frame.flags   = IDK_FRAME_FLAG_VISIBLE;  /* DMABUF bit set by send_dma_buf */
    frame.nfd     = 1;
    frame.stride  = w * 4;
    frame.fourcc  = 0x34324241;  /* DRM_FORMAT_ABGR8888 (Qt RHI GL_RGBA8 default) */
    /* modifier=0 (linear) for Vulkan staging buffer export */

    int fds[4] = {dmabufFd, -1, -1, -1};
    int rc = idk_fs_send_dma_buf(fds, &frame);
    ::close(dmabufFd);

    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);

    if (rc == 0) {
        m_buffer = (m_buffer + 1) % 2;
        m_pending = true;
        m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
        emit frameSent();
        return true;
    }
#endif
    return false;
}

// ── Lazy Vulkan resource initialization ──
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
