#include "input_receiver.h"
#include <QCursor>
#include <QEvent>
#include <QImage>
#include <QPixmap>
#include <QWebEngineView>
#include <QWidget>

#include <cstdlib>
#include <cstring>

#include "core/log.h"

InputReceiver::InputReceiver(const QString &frameSocketPath, QObject *parent) : QObject(parent), m_tp{} {
  const char *env = getenv("IDK_INPUT_ABSTRACT");
  if (env && *env) {
    m_socketPath = QString::fromUtf8(env);
    m_socketAbstract = true;
  } else {
    m_socketPath = frameSocketPath + QStringLiteral("-input");
  }
  m_cursor.magic = IDK_CURSOR_MAGIC;
  m_cursor.version = IDK_CURSOR_VERSION;
  m_cursor.visible = 1;
  m_cursor.shape = IDK_CURSOR_DEFAULT;
  m_cursor.scale = IDK_CURSOR_SCALE_BASE;
}

InputReceiver::~InputReceiver() {
  stopRepeatTimer();
  closeFd();
}

bool InputReceiver::connectToInput() {
  closeFd();

  memset(&m_tp, 0, sizeof(m_tp));
  QByteArray sockName = m_socketAbstract ? (QByteArray(1, '\0') + m_socketPath.toUtf8()) : m_socketPath.toUtf8();
  if (idk_tp_init(&m_tp, IDK_TP_PRODUCER, sockName.constData()) != 0) {
    IDK_LOG("input-rx", "Failed to init transport for %s%s\n", m_socketAbstract ? "\\0" : "",
            m_socketPath.toUtf8().data());
    return false;
  }

  if (!m_tp.ready) {
    IDK_LOG("input-rx", "Transport not ready for %s\n", m_socketPath.toUtf8().data());
    idk_tp_destroy(&m_tp);
    return false;
  }

  int watch_fd = m_tp._client_fd;
  if (m_tp.backend == IDK_TP_SHM) {
    int efd;
    memcpy(&efd, m_tp._rsv + 40, sizeof(efd));
    if (efd <= 0) {
      IDK_ERR("input-rx", "SHM: no eventfd from transport\n");
      idk_tp_destroy(&m_tp);
      return false;
    }
    m_wakeFd = efd;
    watch_fd = m_wakeFd;
  }

  m_notifier = new QSocketNotifier(watch_fd, QSocketNotifier::Read, this);
  connect(m_notifier, &QSocketNotifier::activated, this, &InputReceiver::onReadyRead);

  IDK_LOG("input-rx", "Connected to %s%s (backend=%s, fd=%d)\n", m_socketAbstract ? "\\0" : "",
          m_socketPath.toUtf8().data(), m_tp.backend == IDK_TP_SHM ? "shm" : "socket", watch_fd);
  sendCursor();
  return true;
}

void InputReceiver::disconnect() { closeFd(); }

void InputReceiver::closeFd() {
  if (m_notifier) {
    delete m_notifier;
    m_notifier = nullptr;
  }
  if (m_tp.ready || m_tp._client_fd >= 0 || m_tp._server_fd >= 0) {
    idk_tp_destroy(&m_tp);
  }
  m_wakeFd = -1;
  memset(&m_tp, 0, sizeof(m_tp));
  if (m_captureState) {
    m_captureState = false;
    emit inputCaptureChanged(false);
  }
}

static uint8_t qtCursorShape(Qt::CursorShape shape) {
  switch (shape) {
  case Qt::PointingHandCursor:
    return IDK_CURSOR_POINTER;
  case Qt::WhatsThisCursor:
    return IDK_CURSOR_HELP;
  case Qt::BusyCursor:
    return IDK_CURSOR_PROGRESS;
  case Qt::WaitCursor:
    return IDK_CURSOR_WAIT;
  case Qt::CrossCursor:
    return IDK_CURSOR_CROSSHAIR;
  case Qt::IBeamCursor:
    return IDK_CURSOR_TEXT;
  case Qt::ForbiddenCursor:
    return IDK_CURSOR_NOT_ALLOWED;
  case Qt::OpenHandCursor:
    return IDK_CURSOR_GRAB;
  case Qt::ClosedHandCursor:
    return IDK_CURSOR_GRABBING;
  case Qt::SizeHorCursor:
    return IDK_CURSOR_EW_RESIZE;
  case Qt::SizeVerCursor:
    return IDK_CURSOR_NS_RESIZE;
  case Qt::SizeBDiagCursor:
    return IDK_CURSOR_NESW_RESIZE;
  case Qt::SizeFDiagCursor:
    return IDK_CURSOR_NWSE_RESIZE;
  case Qt::SplitHCursor:
    return IDK_CURSOR_COL_RESIZE;
  case Qt::SplitVCursor:
    return IDK_CURSOR_ROW_RESIZE;
  case Qt::SizeAllCursor:
    return IDK_CURSOR_ALL_RESIZE;
  case Qt::DragCopyCursor:
    return IDK_CURSOR_COPY;
  case Qt::DragMoveCursor:
    return IDK_CURSOR_MOVE;
  case Qt::DragLinkCursor:
    return IDK_CURSOR_ALIAS;
  default:
    return IDK_CURSOR_DEFAULT;
  }
}

void InputReceiver::setWebView(QWebEngineView *view) {
  if (m_webview == view)
    return;
  if (m_webview && m_webview->focusProxy())
    m_webview->focusProxy()->removeEventFilter(this);
  m_webview = view;
  if (m_webview && m_webview->focusProxy()) {
    m_webview->focusProxy()->installEventFilter(this);
    updateCursor(m_webview->focusProxy()->cursor());
  }
}

bool InputReceiver::eventFilter(QObject *obj, QEvent *event) {
  if (m_webview && obj == m_webview->focusProxy() && event->type() == QEvent::CursorChange)
    updateCursor(m_webview->focusProxy()->cursor());
  return QObject::eventFilter(obj, event);
}

void InputReceiver::updateCursor(const QCursor &cursor) {
  m_cursor = {};
  m_cursor.magic = IDK_CURSOR_MAGIC;
  m_cursor.version = IDK_CURSOR_VERSION;
  m_cursor.visible = cursor.shape() == Qt::BlankCursor ? 0 : 1;
  m_cursor.shape = qtCursorShape(cursor.shape());
  m_cursor.scale = IDK_CURSOR_SCALE_BASE;
  m_cursorPixels.clear();

  QPixmap pixmap = cursor.pixmap();
  if (m_cursor.visible && !pixmap.isNull()) {
    QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    QPoint hotspot = cursor.hotSpot();
    if (image.width() > (int)IDK_CURSOR_MAX_DIM || image.height() > (int)IDK_CURSOR_MAX_DIM) {
      const QSize oldSize = image.size();
      image = image.scaled(IDK_CURSOR_MAX_DIM, IDK_CURSOR_MAX_DIM, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      hotspot.setX((int)((int64_t)hotspot.x() * image.width() / oldSize.width()));
      hotspot.setY((int)((int64_t)hotspot.y() * image.height() / oldSize.height()));
    }
    m_cursor.shape = IDK_CURSOR_CUSTOM;
    m_cursor.width = (uint16_t)image.width();
    m_cursor.height = (uint16_t)image.height();
    m_cursor.hotspot_x = (int16_t)hotspot.x();
    m_cursor.hotspot_y = (int16_t)hotspot.y();
    m_cursor.scale = (uint16_t)qBound(1, qRound(pixmap.devicePixelRatio() * IDK_CURSOR_SCALE_BASE), 65535);
    m_cursor.data_size = (uint32_t)image.width() * image.height() * 4u;
    m_cursorPixels = QByteArray(reinterpret_cast<const char *>(image.constBits()), (qsizetype)m_cursor.data_size);
  }
  m_haveCursor = true;
  sendCursor();
}

void InputReceiver::sendCursor() {
  if (!m_haveCursor || !m_tp.ready)
    return;
  const uint8_t *pixels =
      m_cursorPixels.isEmpty() ? nullptr : reinterpret_cast<const uint8_t *>(m_cursorPixels.constData());
  if (idk_tp_send_cursor(&m_tp, &m_cursor, pixels) != 0)
    IDK_LOG("input-rx", "cursor send failed: errno=%d\n", errno);
}
