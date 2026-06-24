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
    RhiTextureExtractor::cleanup();  // Clean up any EGL resources

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
                char ack;
                if (read(ack_fd, &ack, 1) > 0) {
                    m_pending = false;
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
    uchar *memory = (uchar*)m_memory + (PIXELS_SIZE(m_conf.width(), m_conf.height()) * buffer);
    QImage img(memory, m_conf.width(), m_conf.height(), QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    render(&img);


    idk_fs_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.width   = static_cast<uint32_t>(m_conf.width());
    frame.height  = static_cast<uint32_t>(m_conf.height());
    frame.x       = static_cast<uint32_t>(m_conf.x());
    frame.y       = static_cast<uint32_t>(m_conf.y());
    frame.id      = buffer;
    frame.visible = static_cast<uint8_t>(1);
    frame.nfd     = 1;
    frame.type    = IDK_FRAME_TYPE_SHM;

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
    m_memsize = PIXELS_SIZE(m_conf.width(), m_conf.height()) * 2;

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
    Q_UNUSED(this);
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
