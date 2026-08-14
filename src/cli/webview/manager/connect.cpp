#include "input_receiver.h"
#include "manager.h"
#include "webview.h"

#include "core/log.h"
#include "public/idk_producer.h"

Manager::~Manager() {
  stopInputReceiver();
  qDeleteAll(m_views);

  delete m_window;
  delete m_tabBar;
  delete m_statusLabel;

  idk_producer_shutdown();
}

bool Manager::isConnected() const { return idk_producer_is_connected(); }

void Manager::startInputReceiver() {
  if (m_inputRx && m_inputRx->isConnected())
    return;

  if (!m_inputRx) {
    m_inputRx = new InputReceiver(m_socketPath, this);
    connect(m_inputRx, &InputReceiver::inputCaptureChanged, this, &Manager::onInputCaptureChanged);
    connect(m_inputRx, &InputReceiver::overlayVisibleChanged, this, &Manager::onOverlayVisibleChanged);
  }

  if (!m_views.isEmpty())
    m_inputRx->setWebView(m_views.first());

  if (m_inputRx->connectToInput()) {
    IDK_LOG("webview", "input receiver connected to %s-input\n", m_socketPath.toUtf8().data());
    return;
  }

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
        IDK_LOG("webview", "input receiver giving up after 30s - "
                           "game may not be a wayland client\n");
        m_inputRetryTimer->stop();
      }
    });
  }
  m_inputRetries = 0;
  if (!m_inputRetryTimer->isActive()) {
    m_inputRetryTimer->start(2000);
  }
}

void Manager::stopInputReceiver() {
  if (m_inputRetryTimer) {
    m_inputRetryTimer->stop();
  }
  if (m_inputRx) {
    m_inputRx->disconnect();
  }
}
