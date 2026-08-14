#include "input_receiver.h"

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
