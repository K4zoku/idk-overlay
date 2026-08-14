#include "input_receiver.h"
#include "internal.h"

#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWebEngineView>
#include <QWheelEvent>

#include <sys/eventfd.h>

#include "core/log.h"

QWidget *InputReceiver::focusProxy() {
  if (!m_webview)
    return nullptr;
  return m_webview->focusProxy();
}

void InputReceiver::sendFocusIn() {
  QWidget *fp = focusProxy();
  if (!fp)
    return;
  QFocusEvent fin(QEvent::FocusIn, Qt::OtherFocusReason);
  qApp->sendEvent(fp, &fin);
  fp->setFocus(Qt::OtherFocusReason);

  QMouseEvent mv(QEvent::MouseMove, QPointF(m_mouseX, m_mouseY), QPointF(m_mouseX, m_mouseY),
                 QPointF(m_mouseX, m_mouseY), Qt::NoButton, m_buttons, Qt::NoModifier);
  mv.setTimestamp(nowMs());
  qApp->sendEvent(fp, &mv);

  m_focusSent = true;
}

void InputReceiver::onReadyRead() {
  if (!m_tp.ready && m_tp._client_fd < 0 && m_wakeFd < 0)
    return;

  if (m_wakeFd >= 0) {
    eventfd_t val;
    while (eventfd_read(m_wakeFd, &val) == 0) {
    }
  }

  idk_input_event_t ev;
  while (true) {
    int rc = idk_tp_recv_input(&m_tp, &ev);
    if (rc <= 0) {
      if (rc < 0)
        closeFd();
      break;
    }
    if (ev.type < IDK_INPUT_KEY || ev.type > IDK_INPUT_OVERLAY) {
      closeFd();
      return;
    }

    bool nowCaptured = (ev.flags & IDK_INPUT_FLAG_CAPTURE) != 0;
    if (nowCaptured != m_captureState) {
      m_captureState = nowCaptured;
      m_focusSent = false;
      if (!nowCaptured)
        stopRepeatTimer();
      emit inputCaptureChanged(nowCaptured);
      IDK_LOG("input-rx", "capture %s\n", nowCaptured ? "ENABLED" : "DISABLED");
      if (nowCaptured)
        sendFocusIn();
    }
    if (nowCaptured && !m_focusSent)
      sendFocusIn();

    QWidget *fp = focusProxy();
    if (!fp)
      continue;

    quint64 t = nowMs();
    Qt::KeyboardModifiers mods = idkModsToQt(ev.mods);

    switch (ev.type) {
    case IDK_INPUT_KEY: {
      int qtKey = keysymToQtKey(ev.u.key.keysym);
      if (!qtKey)
        break;
      bool isPress = (ev.flags & IDK_INPUT_FLAG_PRESS) != 0;

      QString text;
      if (ev.u.key.keysym >= 0x20 && ev.u.key.keysym < 0x7f)
        text = QString(QChar((uint)ev.u.key.keysym));
      else if (ev.u.key.keysym == 0xff0d)
        text = QStringLiteral("\r");

      if (isPress) {
        if (ev.u.key.keycode != 0 && m_captureState)
          startRepeatTimer(ev.u.key.keycode, ev.u.key.keysym, ev.mods, text);
      } else {
        if (m_repeatKeycode == ev.u.key.keycode)
          stopRepeatTimer();
      }

      auto *qe = new QKeyEvent(isPress ? QEvent::KeyPress : QEvent::KeyRelease, qtKey, mods, text, false);
      qe->setTimestamp(t);
      QCoreApplication::postEvent(fp, qe);
      break;
    }

    case IDK_INPUT_MOTION: {
      m_mouseX = ev.u.motion.x;
      m_mouseY = ev.u.motion.y;
      QPointF local(m_mouseX, m_mouseY);
      auto *me = new QMouseEvent(QEvent::MouseMove, local, local, local, Qt::NoButton, m_buttons, Qt::NoModifier);
      me->setTimestamp(t);
      QCoreApplication::postEvent(fp, me);
      break;
    }

    case IDK_INPUT_BUTTON: {
      m_mouseX = ev.u.btn.x;
      m_mouseY = ev.u.btn.y;
      QPointF local(m_mouseX, m_mouseY);
      Qt::MouseButton sqBtn;
      switch (ev.u.btn.button) {
      case 0x110:
        sqBtn = Qt::LeftButton;
        break;
      case 0x111:
        sqBtn = Qt::RightButton;
        break;
      case 0x112:
        sqBtn = Qt::MiddleButton;
        break;
      case 0x113:
        sqBtn = Qt::XButton1;
        break;
      case 0x114:
        sqBtn = Qt::XButton2;
        break;
      default:
        break;
      }
      bool isPress = (ev.flags & IDK_INPUT_FLAG_PRESS) != 0;
      if (isPress)
        m_buttons |= sqBtn;
      else
        m_buttons &= ~sqBtn;

      auto *be = new QMouseEvent(isPress ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease, local, local, local,
                                 sqBtn, m_buttons, Qt::NoModifier);
      be->setTimestamp(t);
      QCoreApplication::postEvent(fp, be);
      break;
    }

    case IDK_INPUT_AXIS: {
      const int scale = 12;
      int ax = -ev.u.axis.dx * scale;
      int ay = -ev.u.axis.dy * scale;
      QPointF local(m_mouseX, m_mouseY);

      auto postWheel = [&](Qt::ScrollPhase phase, int dy, int dx = 0) {
        auto *we = new QWheelEvent(local, local, QPoint(0, 0), QPoint(dx, dy), m_buttons, Qt::NoModifier, phase, false);
        we->setTimestamp(t);
        QCoreApplication::postEvent(fp, we);
      };
      postWheel(Qt::ScrollBegin, 0);
      if (ax || ay)
        postWheel(Qt::ScrollUpdate, ay, ax);
      postWheel(Qt::ScrollEnd, 0);
      break;
    }

    case IDK_INPUT_REPEAT:
      m_repeatRate = ev.u.repeat.rate > 0 ? ev.u.repeat.rate : 25;
      m_repeatDelay = ev.u.repeat.delay > 0 ? ev.u.repeat.delay : 500;
      IDK_LOG("input-rx", "repeat info: rate=%d cps delay=%d ms\n", m_repeatRate, m_repeatDelay);
      break;

    case IDK_INPUT_OVERLAY: {
      bool visible = ev.u.overlay.visible != 0;
      IDK_LOG("input-rx", "overlay %s\n", visible ? "SHOW" : "HIDE");
      emit overlayVisibleChanged(visible);
      break;
    }
    }
  }
}
