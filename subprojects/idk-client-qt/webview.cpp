#include "webview.h"
#include "manager.h"

#include <QPaintEvent>
#include <QVBoxLayout>
#include <QDialog>
#include <QMenu>

#include <QQuickWidget>
#include <QWebEngineSettings>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFramebufferObject>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QQuickRenderTarget>
// NOTE: Private header #include <QtGui/private/qrhi_p.h> is now only used in RhiTextureExtractor
// This separation allows easier Qt version updates and API changes
#endif

#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "idk_client.h"

// ── Constants ───────────────────────────────────────────────────────────

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
            /* Guard against double-init on reconnect — initMemory() creates
             * a new memfd each time, and the old one would leak. The memfd
             * and mmap persist across socket reconnects (they're independent
             * of the socket fd), so only init once. */
            if (m_memory) {
                fprintf(stderr, "[idk-client-qt] Overlay %u reconnected (memory already init'd, skipping re-init)\n", m_id);
                return;
            }
            initMemory();
            focusProxy()->installEventFilter(this);

            sendCreateImage();
            m_waitReply = false;  /* ready to send first frame */
            m_buffer = 0;
        });
    } else {
        // Already connected, initialize immediately
        initMemory();
        focusProxy()->installEventFilter(this);

        sendCreateImage();
        m_waitReply = false;  /* ready to send first frame */
        m_buffer = 0;
    }

    connect(this, &WebView::loadFinished, this, [this](bool ok) {
        if (!ok || m_conf.url().isEmpty()) {
            return;
        }
        // TODO: Inject script if needed
        // page()->runJavaScript(QStringLiteral("(function(){%1}());").arg(m_conf.injectScript()));
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
        if (m_waitReply || !m_manager->isConnected())
            return QWebEngineView::eventFilter(obj, event);

        // Double-buffer: render to current buffer, then swap
        uint8_t buffer = m_buffer;
        m_buffer = (m_buffer + 1) % 2;
        uchar *memory = (uchar*)m_memory + (PIXELS_SIZE(m_conf.width(), m_conf.height()) * buffer);
        QImage img(memory, m_conf.width(), m_conf.height(), QImage::Format_RGBA8888);
        img.fill(Qt::transparent);
        render(&img);

        // Send frame to idk-overlay
        idk_client_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.width   = static_cast<uint32_t>(m_conf.width());
        frame.height  = static_cast<uint32_t>(m_conf.height());
        frame.x       = static_cast<uint32_t>(m_conf.x());
        frame.y       = static_cast<uint32_t>(m_conf.y());
        frame.id      = buffer;  // tell compositor which double-buffer half has fresh data
        frame.visible = static_cast<uint8_t>(1);
        frame.nfd     = 1;
        frame.type    = IDK_FRAME_TYPE_SHM;

        static int s_frame_count = 0;
        static int s_send_failed = 0;

        /* If send failed before, don't keep trying every frame.
         * Retry every 60 frames (~1/sec at 60fps). */
        if (s_send_failed > 0 && --s_send_failed > 0)
            return QWebEngineView::eventFilter(obj, event);

        int rc = idk_client_send_dma_buf(&m_memfd, &frame);
        if (rc < 0) {
            s_frame_count++;
            if (s_frame_count <= 3 || s_frame_count % 60 == 0) {
                qWarning() << "[idk-client-qt] send frame failed (attempt %d): %s",
                    s_frame_count, strerror(errno);
            }
            s_send_failed = 60;  /* skip next 60 frames */
        } else {
            if (s_frame_count == 0 || s_frame_count % 60 == 0) {
                fprintf(stderr, "[idk-client-qt] frame %d sent OK (%dx%d type=SHM fd=%d)\n",
                        s_frame_count, frame.width, frame.height, m_memfd);
            }
            s_frame_count++;
            emit frameSent();
        }

        /* Wait for compositor ACK — syncs webview to game swap rate.
         * This prevents SHM buffer races and flickering. */
        idk_client_wait_ack();

        /* Request next frame */
        focusProxy()->update();

        return true;
    }
    return QWebEngineView::eventFilter(obj, event);
}

void WebView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu;
    menu.addAction(pageAction(QWebEnginePage::Back));
    menu.addAction(pageAction(QWebEnginePage::Forward));
    menu.addAction(pageAction(QWebEnginePage::Reload));
    menu.addSeparator();
    menu.addAction(pageAction(QWebEnginePage::ViewSource));
    menu.addAction(tr("Inspect"), this, [=]() {
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
    // Allocate double-buffered shared memory for pixel data
    m_memsize = PIXELS_SIZE(m_conf.width(), m_conf.height()) * 2;

    m_memfd = memfd_create("idk-client-qt", MFD_CLOEXEC | MFD_ALLOW_SEALING);
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
    // This slot is called from afterRendering signal when DMA-BUF is available
    // For now, we just use SHM mode since the DMA-BUF path requires
    // QQuickWidget focusProxy setup which QWebEngineView doesn't have
    // This is a placeholder for future DMA-BUF integration
    Q_UNUSED(this);
}

void WebView::sendCreateImage()
{
    // Send "create" message to tell overlay that we're ready
    // This is a no-op for now — the overlay will receive frames via DMA-BUF
    // but we need the proper wire protocol adaptation
    fprintf(stderr, "[idk-client-qt] Overlay %u ready: %dx%d@(%d,%d)\n",
            m_id, m_conf.width(), m_conf.height(), m_conf.x(), m_conf.y());
}

void WebPage::javaScriptConsoleMessage(QWebEnginePage::JavaScriptConsoleMessageLevel level,
                                       const QString &message,
                                       int lineNumber,
                                       const QString &sourceId)
{
    Q_UNUSED(level);
    Q_UNUSED(message);
    Q_UNUSED(lineNumber);
    Q_UNUSED(sourceId);
}
