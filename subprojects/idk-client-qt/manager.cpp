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
                emit socketConnected();
            } else if (state == QLocalSocket::UnconnectedState) {
                emit socketDisconnected();
                m_reconnectTimer->start();
            }
        });

        connect(m_socket, &QLocalSocket::readyRead, this, [this]() {
            QByteArray data = m_socket->readAll();
            if (!data.isEmpty()) {
                // Handle incoming messages from idk-overlay (if needed)
                Q_UNUSED(data);
            }
        });

        connect(m_reconnectTimer, &QTimer::timeout, this, [=]() {
            m_socket->connectToServer(m_socketPath);
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

    m_window->setAttribute(Qt::WA_DontShowOnScreen, tray);
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
    connect(m_tray, &QSystemTrayIcon::activated, this, [=](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            m_window->setAttribute(Qt::WA_DontShowOnScreen, !m_window->testAttribute(Qt::WA_DontShowOnScreen));
            m_window->show();
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
