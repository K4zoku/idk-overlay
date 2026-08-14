#include "input_receiver.h"
#include "internal.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QWidget>

void InputReceiver::startRepeatTimer(uint32_t keycode, uint32_t keysym, uint16_t mods, const QString &text) {
  if (!m_repeatTimer) {
    m_repeatTimer = new QTimer(this);
    m_repeatTimer->setSingleShot(true);
    connect(m_repeatTimer, &QTimer::timeout, this, &InputReceiver::onRepeatTimeout);
  }

  if (m_repeatKeycode != 0 && m_repeatKeycode != keycode)
    stopRepeatTimer();

  m_repeatKeycode = keycode;
  m_repeatKeysym = keysym;
  m_repeatMods = mods;
  m_repeatText = text;
  m_repeatArmed = true;

  m_repeatTimer->start(m_repeatDelay);
}

void InputReceiver::stopRepeatTimer() {
  if (m_repeatTimer)
    m_repeatTimer->stop();
  m_repeatKeycode = 0;
  m_repeatKeysym = 0;
  m_repeatArmed = false;
}

void InputReceiver::onRepeatTimeout() {
  if (!m_captureState || m_repeatKeycode == 0) {
    stopRepeatTimer();
    return;
  }

  QWidget *fp = focusProxy();
  if (!fp) {
    stopRepeatTimer();
    return;
  }

  int qtKey = keysymToQtKey(m_repeatKeysym);
  if (!qtKey)
    return;

  Qt::KeyboardModifiers mods = idkModsToQt(m_repeatMods);
  quint64 t = nowMs();

  QKeyEvent p(QEvent::KeyPress, qtKey, mods, m_repeatText, true);
  p.setTimestamp(t);
  qApp->sendEvent(fp, &p);

  if (m_repeatArmed) {
    m_repeatArmed = false;
    int interval = m_repeatRate > 0 ? (1000 / m_repeatRate) : 40;
    m_repeatTimer->setSingleShot(false);
    m_repeatTimer->start(interval);
  }
}
