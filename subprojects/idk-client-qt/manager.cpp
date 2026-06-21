#include "manager.h"
#include "webview.h"
#include "groupconfig.h"

#include <QDir>
#include <QApplication>
#include <QWebEngineProfile>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QMenu>
#include <QDebug>

#include "idk_client.h"

Manager::Manager(const QString &confFile, bool tray, QObject *parent)
    : QObject(parent)
    , m_settings(new QSettings(
        confFile.isEmpty() ? QDir::homePath() + QLatin1String("/.config/idk-client-qt.conf") : confFile,
        QSettings::IniFormat, this))
    , m_socket(new QLocalSocket(this))
    , m_reconnectTimer(new QTimer(this))
    , m_window(new QWidget())
    , m_tabBar(new QTabBar())
    , m_container(new QWidget())
    , m_statusLabel(new QLabel())
    , m_tray(new QSystemTrayIcon(this))
{
    m_socketPath = resolvePath(m_settings->value("Socket", "/tmp/idk-overlay").toString());

    // Initialize idk_client (reuses existing socket if passed via env)
    const char *reuse_fd = getenv("IDK_SOCKET_FD");
    int fd = reuse_fd ? atoi(reuse_fd) : -1;

    if (idk_client_init(m_socketPath.toUtf8().data(), fd < 0 ? false : true) < 0) {
        fprintf(stderr, "[idk-client-qt] Failed to connect to %s\n", m_socketPath.toUtf8().data());
    }

    // Connect signal — if not using reuse_fd, set up QLocalSocket fallback
    if (fd < 0) {
        connect(m_socket, &QLocalSocket::stateChanged, this, [this](QLocalSocket::LocalSocketState state) {
            if (state == QLocalSocket::ConnectedState) {
                m_disconnect_count = 0;
                fprintf(stderr, "[idk-client-qt] QLocalSocket connected\n");
                /* Re-initialize idk_client's data socket on (re)connect.
                 * idk_client has its own separate socket to the compositor;
                 * if it died (compositor closed, EPIPE on send, etc.) we
                 * need to re-create it so send_dma_buf works again.
                 * This syncs the QLocalSocket status connection with
                 * idk_client's data connection. */
                if (idk_client_get_fd() < 0) {
                    if (idk_client_init(m_socketPath.toUtf8().data(), false) < 0) {
                        fprintf(stderr, "[idk-client-qt] idk_client reconnect failed: %s\n",
                                strerror(errno));
                        m_reconnectTimer->start();
                        return;
                    }
                }
                emit socketConnected();
            } else if (state == QLocalSocket::UnconnectedState) {
                /* Throttle the disconnect log — at 1s reconnect interval,
                 * this fires every second while the compositor is down.
                 * Log first 3, then every 30th (~30s). */
                if (++m_disconnect_count <= 3 || m_disconnect_count % 30 == 0) {
                    fprintf(stderr, "[idk-client-qt] QLocalSocket disconnected (attempt %d)\n",
                            m_disconnect_count);
                }
                /* Mark idk_client's data socket as dead too — isConnected()
                 * now returns false so WebView stops trying to send frames
                 * into a dead socket. Will be re-init'd on next connect. */
                idk_client_shutdown();
                emit socketDisconnected();
                m_reconnectTimer->start();
            }
        });

        connect(m_socket, &QLocalSocket::readyRead, this, [this]() {
            QByteArray data = m_socket->readAll();
            if (!data.isEmpty()) {
                Q_UNUSED(data);
            }
        });

        connect(m_reconnectTimer, &QTimer::timeout, this, [=]() {
            /* Only attempt reconnect if not already connected/connecting */
            if (m_socket->state() == QLocalSocket::UnconnectedState) {
                m_socket->connectToServer(m_socketPath);
            }
        });
        m_reconnectTimer->start(1000);
    } else {
        // Using existing fd, emit connected immediately
        emit socketConnected();
    }

    // Setup UI
    QWebEngineProfile::defaultProfile()->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);

    // Make the window a proper overlay: frameless, topmost, no taskbar entry
    m_window->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    m_window->setAttribute(Qt::WA_TranslucentBackground, true);

    QVBoxLayout *layout = new QVBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_container);
    m_window->setLayout(layout);

    // Default: hidden (offscreen render only). Toggle with tray click.
    m_window->setAttribute(Qt::WA_DontShowOnScreen, true);
    m_window->show();

    // Size and position the window based on the first overlay config
    QTimer::singleShot(200, this, [this]() {
        if (!m_settings) { return; }
        const auto groups = m_settings->childGroups();
        if (groups.isEmpty()) { return; }
        GroupConfig conf(m_settings->fileName(), groups.first());
        int x = conf.x();
        int y = conf.y();
        int w = conf.width();
        int h = conf.height();
        m_window->setGeometry(x, y, w, h);
        m_window->raise();
        m_window->activateWindow();
        qDebug() << "[idk-client-qt] Resized window to" << w << "x" << h << "at" << x << "," << y;
    });

    m_tray->setIcon(QIcon::fromTheme("image-x-generic"));
    m_tray->setToolTip("idk-client-qt");
    m_tray->show();

    // Left-click tray: toggle window visibility
    bool *windowVisible = new bool(false);  // start hidden
    connect(m_tray, &QSystemTrayIcon::activated, this, [=](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            *windowVisible = !*windowVisible;
            m_window->setAttribute(Qt::WA_DontShowOnScreen, !*windowVisible);
            m_window->show();
            if (*windowVisible) {
                m_window->raise();
                m_window->activateWindow();
            }
            qDebug() << "[idk-client-qt] Window" << (*windowVisible ? "shown" : "hidden");
        }
    });

    QMenu *menu = new QMenu();
    menu->addAction("Exit", qApp, &QApplication::quit);
    m_tray->setContextMenu(menu);

    initWebViews();
    updateStatus();
}

Manager::~Manager()
{
    qDeleteAll(m_views);
    idk_client_shutdown();
}

bool Manager::isConnected() const
{
    // Check if idk_client is connected
    return idk_client_get_fd() >= 0;
}

void Manager::onSocketStateChanged(QLocalSocket::LocalSocketState state)
{
    Q_UNUSED(state);
}

void Manager::onSocketReadyRead()
{
    // Handle incoming data
}

void Manager::onReconnectTimer()
{
    m_socket->connectToServer(m_socketPath);
}

void Manager::initWebViews()
{
    uint8_t i = 1;
    const auto groups = m_settings->childGroups();
    for (const QString &group : groups) {
        GroupConfig conf(m_settings->fileName(), group);
        if (conf.url().isEmpty()) {
            qWarning() << "Invalid config" << group;
            continue;
        }
        WebView *view = new WebView(i++, conf, this);
        view->setParent(m_container);
        view->show();
        m_views.append(view);
        m_tabBar->addTab(group);
    }
    if (!m_views.isEmpty()) {
        showView(0);
    }
    qDebug() << "Loaded" << m_views.size() << "views";
}

void Manager::showView(int index)
{
    for (int i = 0; i < m_views.size(); ++i) {
        if (i == index) {
            m_views.at(i)->move(0, 0);
        } else {
            m_views.at(i)->move(9999, 9999);
        }
    }
}

void Manager::updateStatus()
{
    const QString s = isConnected() ? "Connected" : "Connecting...";
    m_statusLabel->setText(QString("Socket: %1 | Status: %2").arg(m_socketPath, s));
}

QString Manager::resolvePath(const QString &path) const
{
    if (QDir::isAbsolutePath(path)) {
        return path;
    }
    return QDir(QDir::cleanPath(QFileInfo(m_settings->fileName()).path())).absoluteFilePath(path);
}
