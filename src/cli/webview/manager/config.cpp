#include "input_receiver.h"
#include "manager.h"
#include "webview.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWebEngineProfile>

#include "core/log.h"
#include "public/idk_producer.h"

static QString resolveConfigPath() {
  const QString xdg = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  return xdg + QStringLiteral("/idk-overlay.conf");
}

Manager::Manager(const QString &confFile, const QString &cliSocketPath, bool noDmaBuf, const QString &cliUrl,
                 int cliWidth, int cliHeight, const QString &cliMatch, QObject *parent)
    : QObject(parent),
      m_settings(new QSettings(confFile.isEmpty() ? resolveConfigPath() : confFile, QSettings::IniFormat, this)),
      m_reconnectTimer(new QTimer(this)), m_noDmaBuf(noDmaBuf), m_cliUrl(cliUrl), m_cliWidth(cliWidth),
      m_cliHeight(cliHeight), m_cliMatch(cliMatch), m_window(new QWidget()), m_tabBar(new QTabBar()),
      m_container(new QWidget()), m_statusLabel(new QLabel()) {
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

  QByteArray sockName = m_socketAbstract ? (QByteArray(1, '\0') + m_socketPath.toUtf8()) : m_socketPath.toUtf8();
  if (idk_producer_init(sockName.constData()) == 0) {
    m_was_connected = true;
    IDK_LOG("webview", "idk_producer connected to %s%s\n", m_socketAbstract ? "\\0" : "", m_socketPath.toUtf8().data());
    emit socketConnected();
    startInputReceiver();
  } else {
    IDK_LOG("webview", "idk_producer connect failed, will retry\n");
  }

  connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
    bool connected_now = idk_producer_is_connected();

    if (connected_now && !m_was_connected) {
      m_disconnect_count = 0;
      IDK_LOG("webview", "idk_producer connected\n");
      emit socketConnected();
      startInputReceiver();
    } else if (!connected_now && m_was_connected) {
      IDK_LOG("webview", "idk_producer disconnected - attempting reconnect\n");
      emit socketDisconnected();
      stopInputReceiver();
      QByteArray sockName = m_socketAbstract ? (QByteArray(1, '\0') + m_socketPath.toUtf8()) : m_socketPath.toUtf8();
      if (idk_producer_init(sockName.constData()) == 0) {
        IDK_LOG("webview", "idk_producer reconnected\n");
        emit socketConnected();
        startInputReceiver();
        connected_now = true;
        m_disconnect_count = 0;
      } else {
        m_disconnect_count = 1;
      }
    } else if (!connected_now && !m_was_connected) {
      m_disconnect_count++;
      bool should_log = (m_disconnect_count == 1 || m_disconnect_count == 5 || m_disconnect_count == 30 ||
                         (m_disconnect_count > 30 && m_disconnect_count % 60 == 0));
      if (should_log) {
        IDK_LOG("webview", "idk_producer waiting for compositor (attempt %d)\n", m_disconnect_count);
      }
      QByteArray sockName = m_socketAbstract ? (QByteArray(1, '\0') + m_socketPath.toUtf8()) : m_socketPath.toUtf8();
      if (idk_producer_init(sockName.constData()) == 0) {
        IDK_LOG("webview", "idk_producer connected after %d attempts\n", m_disconnect_count);
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
  if (m_inputRx && !m_views.isEmpty())
    m_inputRx->setWebView(m_views.first());
  updateStatus();
}

void Manager::updateStatus() {
  const QString s = isConnected() ? "Connected" : "Connecting...";
  m_statusLabel->setText(QString("Socket: %1 | Status: %2").arg(m_socketPath, s));
}

QString Manager::resolvePath(const QString &path) const {
  if (QDir::isAbsolutePath(path)) {
    return path;
  }
  return QDir(QDir::cleanPath(QFileInfo(m_settings->fileName()).path())).absoluteFilePath(path);
}
