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
#include "idk_log.h"

Manager::Manager(const QString &confFile, bool tray, QObject *parent)
    : QObject(parent)
    , m_settings(new QSettings(
        confFile.isEmpty() ? QDir::homePath() + QLatin1String("/.config/idk-client-qt.conf") : confFile,
        QSettings::IniFormat, this))
    , m_reconnectTimer(new QTimer(this))
    , m_window(new QWidget())
    , m_tabBar(new QTabBar())
    , m_container(new QWidget())
    , m_statusLabel(new QLabel())
    , m_tray(new QSystemTrayIcon(this))
{
    m_socketPath = resolvePath(m_settings->value("Socket", "/tmp/idk-overlay").toString());

    /* ── Connection strategy ───────────────────────────────────────────
     * OLD design (commit 525c106 and earlier) used TWO sockets to the
     * compositor: idk_client's internal g_sock_fd (for data) + a
     * QLocalSocket m_socket (for status). The compositor only accepts
     * ONE client, so whichever socket connect()'d first got accept()'d.
     * If QLocalSocket won the race, idk_client's frames went into a
     * dead queue and the overlay never showed — this was the
     * "start client first → no overlay" bug.
     *
     * NEW design: use idk_client as the SOLE connection. A QTimer polls
     * idk_client_get_fd() every 1s to detect (dis)connect transitions
     * and emit socketConnected / socketDisconnected. No QLocalSocket —
     * no race, no dual-socket confusion. */
    const char *reuse_fd = getenv("IDK_SOCKET_FD");
    int fd = reuse_fd ? atoi(reuse_fd) : -1;
    m_was_connected = false;

    if (fd >= 0) {
        /* Reusing existing fd — idk_client_init stores it, no connect() */
        if (idk_client_init(m_socketPath.toUtf8().data(), true) == 0) {
            m_was_connected = true;
            emit socketConnected();
        }
    } else {
        /* Try to connect now; if it fails, the reconnect timer will retry.
         * Uses blocking init with 30 retries (~3s) for the initial attempt —
         * handles the common case where compositor is already running. */
        if (idk_client_init(m_socketPath.toUtf8().data(), false) == 0) {
            m_was_connected = true;
            IDK_LOG("client-qt", "idk_client connected to %s\n",
                    m_socketPath.toUtf8().data());
            emit socketConnected();
        } else {
            IDK_LOG("client-qt", "idk_client connect failed, will retry\n");
        }

        /* Poll idk_client_get_fd() every 1s to detect state transitions. */
        connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
            int fd_now = idk_client_get_fd();
            bool connected_now = (fd_now >= 0);

            if (connected_now && !m_was_connected) {
                /* Transition: disconnected → connected */
                m_disconnect_count = 0;
                IDK_LOG("client-qt", "idk_client connected (fd=%d)\n", fd_now);
                emit socketConnected();
            } else if (!connected_now && m_was_connected) {
                /* Transition: connected → disconnected.
                 * idk_client's fd went dead (compositor closed, EPIPE, etc.).
                 * Try to reconnect; if it fails, the next timer tick retries.
                 * Log only ONCE per disconnect (not every retry) — the
                 * "still connecting" branch below handles subsequent retries. */
                IDK_LOG("client-qt", "idk_client disconnected — attempting reconnect\n");
                emit socketDisconnected();
                /* Attempt non-blocking reconnect (single attempt, no 3s block) */
                if (idk_client_init2(m_socketPath.toUtf8().data(), false, 0) == 0) {
                    IDK_LOG("client-qt", "idk_client reconnected\n");
                    emit socketConnected();
                    connected_now = true;
                    m_disconnect_count = 0;
                } else {
                    /* Start counting retries for the "still connecting" branch */
                    m_disconnect_count = 1;
                }
            } else if (!connected_now && !m_was_connected) {
                /* Still never connected — retry with non-blocking init.
                 * This handles the "client-first, game-after" scenario.
                 * LOG SPARSELY: only at attempts 1, 5, 30, then every 60
                 * (~1min). At 1s timer interval this means:
                 *   attempt 1 (1s)  — first retry
                 *   attempt 5 (5s)  — "still trying"
                 *   attempt 30 (30s) — "still trying"
                 *   attempt 60, 120, ... (1min, 2min) — heartbeat
                 * Silent in between — no log spam while waiting for the
                 * compositor to come up. */
                m_disconnect_count++;
                bool should_log = (m_disconnect_count == 1 ||
                                   m_disconnect_count == 5 ||
                                   m_disconnect_count == 30 ||
                                   (m_disconnect_count > 30 && m_disconnect_count % 60 == 0));
                if (should_log) {
                    IDK_LOG("client-qt", "idk_client waiting for compositor (attempt %d)\n",
                            m_disconnect_count);
                }
                if (idk_client_init2(m_socketPath.toUtf8().data(), false, 0) == 0) {
                    IDK_LOG("client-qt", "idk_client connected after %d attempts\n",
                            m_disconnect_count);
                    m_disconnect_count = 0;
                    emit socketConnected();
                    connected_now = true;
                }
            }
            m_was_connected = connected_now;
        });
        m_reconnectTimer->start(1000);
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

void Manager::onSocketReadyRead()
{
    // Handle incoming data — currently unused (idk_client handles data)
}

void Manager::onReconnectTimer()
{
    // Timer callback now handled by lambda in constructor
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
