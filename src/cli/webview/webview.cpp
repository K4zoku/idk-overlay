#include "webview.h"
#include "manager.h"

#include <QPaintEvent>
#include <QVBoxLayout>
#include <QDialog>
#include <QMenu>
#include <QDateTime>

#include <QQuickWidget>
#include <QWebEngineSettings>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFramebufferObject>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QQuickRenderTarget>

#endif

#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>

#include "public/idk_fs.h"
#include "core/log.h"





#define PIXELS_SIZE(w, h)  ((w) * (h) * 4)

WebView::WebView(uint8_t id, const GroupConfig &conf, Manager *manager, QWidget *parent)
    : QWebEngineView(parent)
    , m_id(id)
    , m_conf(conf)
    , m_manager(manager)
{
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
                IDK_LOG("client-qt", "Overlay %u reconnected (memory already init'd, skipping re-init)\n", m_id);

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
    RhiTextureExtractor::cleanup();

    if (m_eglCtx != EGL_NO_CONTEXT) {
        eglDestroyContext(m_eglDpy, m_eglCtx);
    }
    if (m_eglSurf != EGL_NO_SURFACE) {
        eglDestroySurface(m_eglDpy, m_eglSurf);
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
                        IDK_LOG("client-qt", "compositor rejected DMABUF, falling back to SHM\n");
                        m_dmaBufFailed = true;
                    }
                    if (ack_msg.w > 0 && ack_msg.h > 0) {
                        IDK_LOG("client-qt", "ACK received with game size: %dx%d\n",
                                ack_msg.w, ack_msg.h);
                        resizeForGame(ack_msg.w, ack_msg.h);
                    }
                }
            }
        }
        int now = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
        if (m_pending && (now - m_sendTime) > 100) {
            IDK_LOG("client-qt", "ACK timeout (%dms) — force-unlock pending\n",
                    now - m_sendTime);
            m_pending = false;
        }
        if (m_pending) {
            QTimer::singleShot(4, this, [this]() { doRenderAndSend(); });
            return;
        }
    }


    uint8_t buffer = m_buffer;
    m_buffer = (m_buffer + 1) % 2;
    uchar *memory = (uchar*)m_memory + (PIXELS_SIZE(m_renderW, m_renderH) * buffer);
    QImage img(memory, m_renderW, m_renderH, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    render(&img);


    idk_fs_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.width   = static_cast<uint32_t>(m_renderW);
    frame.height  = static_cast<uint32_t>(m_renderH);
    frame.id      = buffer;
    frame.visible = static_cast<uint8_t>(1);
    frame.nfd     = 1;
    frame.type    = IDK_FRAME_TYPE_SHM;

    /* ── Try DMABUF export if available ── */
    if (m_useDmaBuf && !m_dmaBufFailed && m_eglCtx != EGL_NO_CONTEXT) {
        GLuint exportTex = 0;
        int exportFds[4] = {-1, -1, -1, -1};
        EGLint exportStrides[4] = {0};
        EGLint exportOffsets[4] = {0};
        int exportNfd = 0, exportFourcc = 0;
        bool dmabufOk = false;

        EGLDisplay savedDpy = eglGetCurrentDisplay();
        EGLContext savedCtx = eglGetCurrentContext();
        EGLSurface savedRead = eglGetCurrentSurface(EGL_READ);
        EGLSurface savedDraw = eglGetCurrentSurface(EGL_DRAW);

        if (eglMakeCurrent(m_eglDpy, m_eglSurf, m_eglSurf, m_eglCtx)) {
            glGenTextures(1, &exportTex);
            glBindTexture(GL_TEXTURE_2D, exportTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         (GLsizei)m_conf.width(), (GLsizei)m_conf.height(), 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, memory);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);

            EGLImageKHR eglImg = eglCreateImage(
                m_eglDpy, m_eglCtx, EGL_GL_TEXTURE_2D,
                reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(exportTex)),
                nullptr);

            if (eglImg) {
                auto queryFn = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>(
                    eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
                auto exportFn = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(
                    eglGetProcAddress("eglExportDMABUFImageMESA"));

                if (queryFn && exportFn) {
                    EGLuint64KHR modifier = 0;
                    if (queryFn(m_eglDpy, eglImg, &exportFourcc, &exportNfd, &modifier)) {
                        if (exportFn(m_eglDpy, eglImg, exportFds, exportStrides, exportOffsets)) {
                            dmabufOk = true;
                        } else {
                            IDK_LOG("client-qt", "dmabuf export failed (0x%x)\n", eglGetError());
                        }
                    } else {
                        IDK_LOG("client-qt", "dmabuf query failed (0x%x)\n", eglGetError());
                    }
                } else {
                    IDK_LOG("client-qt", "dmabuf functions not found\n");
                }
                eglDestroyImage(m_eglDpy, eglImg);
            } else {
                IDK_LOG("client-qt", "eglCreateImage failed (0x%x)\n", eglGetError());
            }

            if (!dmabufOk) {
                glDeleteTextures(1, &exportTex);
                exportTex = 0;
            }
        }

        if (savedDpy != EGL_NO_DISPLAY) {
            eglMakeCurrent(savedDpy, savedDraw, savedRead, savedCtx);
        }

        if (dmabufOk && exportNfd > 0) {
            frame.type  = IDK_FRAME_TYPE_DMABUF;
            frame.stride = static_cast<uint32_t>(exportStrides[0]);
            frame.format = static_cast<uint32_t>(exportFourcc);
            frame.nfd    = static_cast<uint8_t>(exportNfd);

            int rc = idk_fs_send_dma_buf(exportFds, &frame);

            for (int i = 0; i < exportNfd; i++) {
                if (exportFds[i] >= 0) ::close(exportFds[i]);
            }
            glDeleteTextures(1, &exportTex);

            if (rc == 0) {
                IDK_LOG("client-qt", "frame sent OK (%dx%d type=DMABUF "
                        "fourcc=0x%x stride=%d nfd=%d)\n",
                        frame.width, frame.height, exportFourcc,
                        exportStrides[0], exportNfd);
                m_pending = true;
                m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
                m_dmaBufFailed = false;
                emit frameSent();
                return;
            }
        }

        /* DMABUF failed — fall through to SHM */
        m_dmaBufFailed = true;
        IDK_LOG("client-qt", "dmabuf failed, falling back to SHM\n");
    }

    /* ── SHM fallback ── */
    frame.type = IDK_FRAME_TYPE_SHM;
    frame.stride = 0;
    frame.format = 0;
    frame.nfd    = 1;

    static int s_consecutive_failures = 0;

    int rc = idk_fs_send_dma_buf(&m_memfd, &frame);
    if (rc < 0) {
        s_consecutive_failures++;
        if (s_consecutive_failures <= 3 || s_consecutive_failures % 60 == 0) {
            qWarning("[idk-webview] send failed (attempt %d): %s",
                     s_consecutive_failures, strerror(errno));
        }
        if (s_consecutive_failures > 300) {
            IDK_LOG("client-qt", "Too many consecutive send failures — "
                    "forcing disconnect\n");
            idk_fs_shutdown();
            s_consecutive_failures = 0;
        }
        QTimer::singleShot(16, this, [this]() { doRenderAndSend(); });
        return;
    }
    s_consecutive_failures = 0;

    IDK_LOG("client-qt", "frame sent OK (%dx%d type=SHM fd=%d)\n",
            frame.width, frame.height, m_memfd);
    emit frameSent();

    m_pending = true;
    m_sendTime = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
}

void WebView::resizeForGame(int w, int h)
{
    if (w == m_renderW && h == m_renderH) return;

    IDK_LOG("client-qt", "game resize: %dx%d -> %dx%d\n",
            m_renderW, m_renderH, w, h);

    setMinimumSize(w, h);
    setMaximumSize(w, h);
    resize(w, h);

    /* Re-allocate SHM memory for new size */
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
    QTimer::singleShot(0, this, [this]() { doRenderAndSend(); });
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
        IDK_LOG("client-qt", "initDmaBuf: eglGetDisplay failed, dmabuf disabled\n");
        m_useDmaBuf = false;
        return;
    }

    EGLint major, minor;
    if (!eglInitialize(m_eglDpy, &major, &minor)) {
        IDK_LOG("client-qt", "initDmaBuf: eglInitialize failed, dmabuf disabled\n");
        m_eglDpy = EGL_NO_DISPLAY;
        m_useDmaBuf = false;
        return;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        IDK_LOG("client-qt", "initDmaBuf: eglBindAPI failed, dmabuf disabled\n");
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
        IDK_LOG("client-qt", "initDmaBuf: eglChooseConfig failed, dmabuf disabled\n");
        eglTerminate(m_eglDpy);
        m_eglDpy = EGL_NO_DISPLAY;
        m_useDmaBuf = false;
        return;
    }

    static const EGLint pbuf_attribs[] = {
        EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE
    };
    m_eglSurf = eglCreatePbufferSurface(m_eglDpy, config, pbuf_attribs);
    if (m_eglSurf == EGL_NO_SURFACE) {
        IDK_LOG("client-qt", "initDmaBuf: eglCreatePbufferSurface failed, dmabuf disabled\n");
        eglTerminate(m_eglDpy);
        m_eglDpy = EGL_NO_DISPLAY;
        m_useDmaBuf = false;
        return;
    }

    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    m_eglCtx = eglCreateContext(m_eglDpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (m_eglCtx == EGL_NO_CONTEXT) {
        IDK_LOG("client-qt", "initDmaBuf: eglCreateContext failed, dmabuf disabled\n");
        eglDestroySurface(m_eglDpy, m_eglSurf);
        m_eglSurf = EGL_NO_SURFACE;
        eglTerminate(m_eglDpy);
        m_eglDpy = EGL_NO_DISPLAY;
        m_useDmaBuf = false;
        return;
    }

    IDK_LOG("client-qt", "initDmaBuf: EGL context ready for dmabuf export\n");
}

void WebView::sendCreateImage()
{
    IDK_LOG("client-qt", "Overlay %u ready: %dx%d@(%d,%d)\n",
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
