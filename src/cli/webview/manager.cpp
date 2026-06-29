#include "manager.h"
#include "webview.h"
#include "groupconfig.h"
#include "input_receiver.h"

#include <QDir>
#include <QStandardPaths>
#include <QApplication>
#include <QWebEngineProfile>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QMenu>
#include <QDebug>
#include <QRegularExpression>

#include <unistd.h>

#include "public/idk_fs.h"
#include "core/log.h"

static QString resolveConfigPath()
{
    const QString xdg = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return xdg + QStringLiteral("/idk-overlay.conf");
}

Manager::Manager(const QString &confFile,
                 const QString &cliSocketPath,
                 bool noDmaBuf,
                 const QString &cliUrl,
                 int cliWidth,
                 int cliHeight,
                 const QString &cliMatch,
                 QObject *parent)
    : QObject(parent)
    , m_settings(new QSettings(
        confFile.isEmpty() ? resolveConfigPath() : confFile,
        QSettings::IniFormat, this))
    , m_reconnectTimer(new QTimer(this))
    , m_noDmaBuf(noDmaBuf)
    , m_cliUrl(cliUrl)
    , m_cliWidth(cliWidth)
    , m_cliHeight(cliHeight)
    , m_cliMatch(cliMatch)
    , m_window(new QWidget())            /* parented in body via setWindowFlags/show */
    , m_tabBar(new QTabBar())
    , m_container(new QWidget())
    , m_statusLabel(new QLabel())
{
    /* Socket path priority:
     *   1. IDK_TP_ABSTRACT env (broker-mode: abstract AF_UNIX name, no \0)
     *   2. CLI --socket
     *   3. IDK_SOCKET env / fail
     * Socket is no longer in config - it's always dynamic. */
    const char *envTpAbstract = getenv("IDK_TP_ABSTRACT");
    if (envTpAbstract && *envTpAbstract) {
        m_socketPath = QString::fromUtf8(envTpAbstract);
        m_socketAbstract = true;
    } else if (!cliSocketPath.isEmpty()) {
        m_socketPath = cliSocketPath;
    } else {
        const char *envSocket = getenv("IDK_SOCKET");
        if (envSocket && *envSocket) {
            m_socketPath = QString::fromUtf8(envSocket);
        } else {
            qCritical("idk-webview must be launched by the game (fork+exec).");
            qCritical("Run your game with LD_PRELOAD=libidk-overlay.so instead.");
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
            return;
        }
    }

    m_was_connected = false;

    QByteArray sockName = m_socketAbstract
        ? (QByteArray(1, '\0') + m_socketPath.toUtf8())
        : m_socketPath.toUtf8();
    if (idk_fs_init(sockName.constData()) == 0) {
        m_was_connected = true;
        IDK_LOG("webview", "idk_fs connected to %s%s\n",
                m_socketAbstract ? "\\0" : "",
                m_socketPath.toUtf8().data());
        emit socketConnected();
        startInputReceiver();
    } else {
        IDK_LOG("webview", "idk_fs connect failed, will retry\n");
    }

    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        bool connected_now = idk_fs_is_connected();

        if (connected_now && !m_was_connected) {
            m_disconnect_count = 0;
            IDK_LOG("webview", "idk_fs connected\n");
            emit socketConnected();
            startInputReceiver();
        } else if (!connected_now && m_was_connected) {
            IDK_LOG("webview", "idk_fs disconnected - attempting reconnect\n");
            emit socketDisconnected();
            stopInputReceiver();
            QByteArray sockName = m_socketAbstract
                ? (QByteArray(1, '\0') + m_socketPath.toUtf8())
                : m_socketPath.toUtf8();
            if (idk_fs_init(sockName.constData()) == 0) {
                IDK_LOG("webview", "idk_fs reconnected\n");
                emit socketConnected();
                startInputReceiver();
                connected_now = true;
                m_disconnect_count = 0;
            } else {
                m_disconnect_count = 1;
            }
        } else if (!connected_now && !m_was_connected) {
            m_disconnect_count++;
            bool should_log = (m_disconnect_count == 1 ||
                               m_disconnect_count == 5 ||
                               m_disconnect_count == 30 ||
                               (m_disconnect_count > 30 && m_disconnect_count % 60 == 0));
            if (should_log) {
                IDK_LOG("webview", "idk_fs waiting for compositor (attempt %d)\n",
                        m_disconnect_count);
            }
            QByteArray sockName = m_socketAbstract
                ? (QByteArray(1, '\0') + m_socketPath.toUtf8())
                : m_socketPath.toUtf8();
            if (idk_fs_init(sockName.constData()) == 0) {
                IDK_LOG("webview", "idk_fs connected after %d attempts\n",
                        m_disconnect_count);
                m_disconnect_count = 0;
                emit socketConnected();
                startInputReceiver();
                connected_now = true;
            }
        }
        m_was_connected = connected_now;
    });
    m_reconnectTimer->start(1000);

    QWebEngineProfile::defaultProfile()->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);

    // Make the window a proper overlay: frameless, topmost, no taskbar entry
    m_window->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    m_window->setAttribute(Qt::WA_TranslucentBackground, true);

    QVBoxLayout *layout = new QVBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_container);
    m_window->setLayout(layout);

    m_window->winId();
    m_window->setAttribute(Qt::WA_DontShowOnScreen, true);
    m_window->show();

    initWebViews();
    /* startInputReceiver() may have been called before initWebViews()
     * (during socket connect in the constructor) - if so, m_webview
     * was never set on InputReceiver because m_views was empty. */
    if (m_inputRx && !m_views.isEmpty())
        m_inputRx->setWebView(m_views.first());
    updateStatus();
}

Manager::~Manager()
{
    stopInputReceiver();
    qDeleteAll(m_views);

    delete m_window;
    delete m_tabBar;
    delete m_statusLabel;

    idk_fs_shutdown();
}

bool Manager::isConnected() const
{
    return idk_fs_is_connected();
}

void Manager::initWebViews()
{
    uint8_t i = 1;
    const auto groups = m_settings->childGroups();

    /* Determine process name for config section matching.
     * Priority: CLI --match > IDK_MATCH env > /proc/$PPID/comm (when forked).
     * When the injected lib forks webview, it passes --match <comm>. */
    QString processName = m_cliMatch;
    if (processName.isEmpty()) {
        const char *envMatch = getenv("IDK_MATCH");
        if (envMatch && *envMatch) processName = QString::fromUtf8(envMatch);
    }

    /* If CLI url is set, create a single overlay from CLI args (bypass config). */
    if (!m_cliUrl.isEmpty()) {
        GroupConfig conf(m_cliUrl, m_cliWidth > 0 ? m_cliWidth : 1280,
                         m_cliHeight > 0 ? m_cliHeight : 720);
        WebView *view = new WebView(i++, conf, this, m_noDmaBuf);
        view->setParent(m_container);
        view->show();
        m_views.append(view);
        m_tabBar->addTab("CLI");
        if (!m_views.isEmpty()) showView(0);
        qDebug() << "Loaded 1 view from CLI args";
        return;
    }

    /* Iterate config sections. If Match= regex is set, only load sections
     * whose Match matches the process name. If no Match= in any section,
     * load all (backwards compat). */
    QRegularExpression re;
    bool hasMatchSections = false;
    if (!processName.isEmpty()) {
        /* Check if any section has Match= */
        for (const QString &group : groups) {
            QSettings s(m_settings->fileName(), QSettings::IniFormat);
            s.beginGroup(group);
            if (s.contains("Match")) { hasMatchSections = true; break; }
            s.endGroup();
        }
        if (hasMatchSections) {
            re.setPattern(processName);
        }
    }

    for (const QString &group : groups) {
        GroupConfig conf(m_settings->fileName(), group);

        /* If Match= regex sections exist, filter by process name */
        if (hasMatchSections && re.isValid()) {
            QString matchPattern = conf.match();
            if (!matchPattern.isEmpty()) {
                QRegularExpression sectionRe(matchPattern);
                if (!sectionRe.isValid() || !sectionRe.match(processName).hasMatch()) {
                    continue;  /* skip - doesn't match */
                }
            } else {
                continue;  /* skip sections without Match= when filtering */
            }
        }

        if (conf.url().isEmpty()) {
            qWarning() << "Invalid config" << group;
            continue;
        }
        WebView *view = new WebView(i++, conf, this, m_noDmaBuf);
        view->setParent(m_container);
        view->show();
        m_views.append(view);
        m_tabBar->addTab(group);
    }
    if (!m_views.isEmpty()) {
        showView(0);
    }
    qDebug() << "Loaded" << m_views.size() << "views"
             << (hasMatchSections ? QStringLiteral("(matched '%1')").arg(processName) : QString());
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

void Manager::startInputReceiver()
{
    if (m_inputRx && m_inputRx->isConnected()) return;

    if (!m_inputRx) {
        m_inputRx = new InputReceiver(m_socketPath, this);
        connect(m_inputRx, &InputReceiver::inputCaptureChanged,
                this, &Manager::onInputCaptureChanged);
        connect(m_inputRx, &InputReceiver::overlayVisibleChanged,
                this, &Manager::onOverlayVisibleChanged);
    }

    /* Always refresh the webview - may be called before initWebViews()
     * (where m_views is still empty) or from a reconnect timer (where
     * m_views is already populated). */
    if (!m_views.isEmpty())
        m_inputRx->setWebView(m_views.first());

    if (m_inputRx->connectToInput()) {
        IDK_LOG("webview", "input receiver connected to %s-input\n",
                m_socketPath.toUtf8().data());
        return;
    }

    /* Connection failed - retry every 2s for 30s, then give up.
     * Use a member counter instead of a heap-allocated int captured by
     * the lambda - if the Manager is destroyed while the retry timer
     * is still active, the captured 'new int' leaks. The member is
     * cleaned up automatically when the Manager is destroyed. */
    if (!m_inputRetryTimer) {
        m_inputRetryTimer = new QTimer(this);
        m_inputRetryTimer->setSingleShot(false);
        connect(m_inputRetryTimer, &QTimer::timeout, this, [this]() {
            m_inputRetries++;
            if (m_inputRx && m_inputRx->connectToInput()) {
                IDK_LOG("webview", "input receiver connected after %d retries\n", m_inputRetries);
                m_inputRetryTimer->stop();
                return;
            }
            if (m_inputRetries >= 15) {
                /* Give up after 30s - no input hook available */
                IDK_LOG("webview", "input receiver giving up after 30s - "
                        "game may not be a wayland client\n");
                m_inputRetryTimer->stop();
            }
        });
    }
    m_inputRetries = 0;  /* reset counter on each (re)start */
    if (!m_inputRetryTimer->isActive()) {
        m_inputRetryTimer->start(2000);
    }
}

void Manager::stopInputReceiver()
{
    if (m_inputRetryTimer) {
        m_inputRetryTimer->stop();
    }
    if (m_inputRx) {
        m_inputRx->disconnect();
    }
}

void Manager::onInputCaptureChanged(bool captured)
{
    emit inputCaptureChanged(captured);

    if (m_views.isEmpty()) return;

    QStringList events;
    if (captured && !m_lastCaptureState)
        events << QStringLiteral("new CustomEvent('overlaycapturestart')");
    else if (!captured && m_lastCaptureState)
        events << QStringLiteral("new CustomEvent('overlaycaptureend')");
    events << QStringLiteral("new CustomEvent('overlaycapturechanged',{detail:{captured:%1}})").arg(captured ? "true" : "false");
    m_lastCaptureState = captured;

    QString js = QStringLiteral(
        "(function(){"
        "var evts=[%1];"
        "for(var i=0;i<evts.length;i++)window.dispatchEvent(evts[i]);"
        "})()").arg(events.join(","));
    for (auto *view : m_views) {
        if (view->page())
            view->page()->runJavaScript(js);
    }
}

void Manager::onOverlayVisibleChanged(bool visible)
{
    emit overlayVisibleChanged(visible);

    if (m_views.isEmpty()) return;

    QStringList events;
    if (visible && !m_lastVisibleState)
        events << QStringLiteral("new CustomEvent('overlayshow')");
    else if (!visible && m_lastVisibleState)
        events << QStringLiteral("new CustomEvent('overlayhide')");
    events << QStringLiteral("new CustomEvent('overlayvisiblechanged',{detail:{visible:%1}})").arg(visible ? "true" : "false");
    m_lastVisibleState = visible;

    QString js = QStringLiteral(
        "(function(){"
        "var evts=[%1];"
        "for(var i=0;i<evts.length;i++)window.dispatchEvent(evts[i]);"
        "})()").arg(events.join(","));
    for (auto *view : m_views) {
        if (view->page())
            view->page()->runJavaScript(js);
    }
}
