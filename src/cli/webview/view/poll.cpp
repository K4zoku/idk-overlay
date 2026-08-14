#include "webview.h"

#include <QDateTime>
#include <QDebug>

#include "core/log.h"
#include "public/idk_producer.h"

void WebView::processAck(const idk_ack_msg_t &ack_msg) {
  if (ack_msg.ack == 1) {
    m_dmabufRejectCount++;
    if (m_dmabufRejectCount >= 5 && !m_dmaBufFailed) {
      IDK_LOG("webview-qt", "compositor rejected DMABUF %d times - falling back to SHM\n", m_dmabufRejectCount);
      m_dmaBufFailed = true;
    } else if (!m_dmaBufFailed) {
      IDK_LOG("webview-qt", "compositor rejected DMABUF (%d/5) - will retry\n", m_dmabufRejectCount);
    }
  } else {
    if (m_dmabufRejectCount > 0) {
      IDK_LOG("webview-qt", "DMABUF accepted after %d rejection(s) - counter reset\n", m_dmabufRejectCount);
      m_dmabufRejectCount = 0;
    }
  }
  if (ack_msg.w > 0 && ack_msg.h > 0) {
    IDK_LOG("webview-qt", "ACK received with game size: %dx%d\n", ack_msg.w, ack_msg.h);
    resizeForGame(ack_msg.w, ack_msg.h);
    m_resizePending = true;
  }
}

bool WebView::pollAck() {
  if (!m_pending || !m_manager->isConnected()) {
    m_pending = false;
    m_ackPollTimer->stop();
    return false;
  }

  idk_ack_msg_t ack_msg;
  if (idk_producer_wait_ack(&ack_msg, 0) == 0) {
    m_pending = false;
    processAck(ack_msg);
    m_requestTimer->start(16);
    return true;
  }

  int now = QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF;
  if ((now - m_sendTime) > 100) {
    m_pending = false;
    IDK_LOG("webview-qt", "ACK timeout (%dms) - force-unlock pending\n", now - m_sendTime);
    m_requestTimer->start(16);
    return false;
  }

  m_ackPollTimer->start(16);
  return false;
}

void WebView::onRequestReceived() {
  if (m_pending || !m_manager->isConnected())
    return;

  idk_request_msg_t req;
  if (idk_producer_recv_request(&req, 0) == 0 && req.type == IDK_REQUEST_NEXT_FRAME) {
    m_requestTimer->stop();
    if (auto *fp = focusProxy())
      fp->update();
    return;
  }

  m_requestTimer->start(16);
}

void WebView::onOverlayVisibleChanged(bool visible) {
  if (m_overlayVisible == visible)
    return;
  m_overlayVisible = visible;
  IDK_LOG("webview-qt", "overlay %s - %s timers\n", visible ? "SHOW" : "HIDE", visible ? "restarting" : "stopping");

  if (!visible) {
    /* Stop the ACK/REQUEST poll loop. The compositor drops in-flight
     * frames on hide without ACKing, so m_pending is cleared by the
     * ACK-timeout path on the next visible transition. Force-clear it
     * now so doRenderAndSend() can run immediately when visible again. */
    m_ackPollTimer->stop();
    m_requestTimer->stop();
    m_pending = false;
  } else {
    /* Overlay visible again - kick a render immediately. */
    if (auto *fp = focusProxy())
      fp->update();
  }
}

void WebPage::javaScriptConsoleMessage(QWebEnginePage::JavaScriptConsoleMessageLevel level, const QString &message,
                                       int lineNumber, const QString &sourceId) {
  Q_UNUSED(level);
  Q_UNUSED(sourceId);
  if (message.startsWith(QLatin1String("idk:")))
    qDebug() << "[idk-js]" << message << "at line" << lineNumber;
}
