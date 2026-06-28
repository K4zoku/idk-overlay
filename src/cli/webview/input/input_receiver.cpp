#include "input_receiver.h"

#include <QSocketNotifier>
#include <QDebug>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFocusEvent>
#include <QDateTime>
#include <QGuiApplication>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <thread>

#include "public/idk_ipc.h"
#include "core/log.h"

struct SymQtEntry { quint32 sym; int qtKey; };
static const SymQtEntry SYM_QT[] = {
    { 0xff08, Qt::Key_Backspace },
    { 0xff09, Qt::Key_Tab       },
    { 0xff0d, Qt::Key_Return    },
    { 0xff1b, Qt::Key_Escape    },
    { 0xff50, Qt::Key_Home      },
    { 0xff51, Qt::Key_Left      },
    { 0xff52, Qt::Key_Up        },
    { 0xff53, Qt::Key_Right     },
    { 0xff54, Qt::Key_Down      },
    { 0xff55, Qt::Key_PageUp    },
    { 0xff56, Qt::Key_PageDown  },
    { 0xff57, Qt::Key_End       },
    { 0xff63, Qt::Key_Insert    },
    { 0xffff, Qt::Key_Delete    },
    { 0xffbe, Qt::Key_F1  }, { 0xffbf, Qt::Key_F2  }, { 0xffc0, Qt::Key_F3  },
    { 0xffc1, Qt::Key_F4  }, { 0xffc2, Qt::Key_F5  }, { 0xffc3, Qt::Key_F6  },
    { 0xffc4, Qt::Key_F7  }, { 0xffc5, Qt::Key_F8  }, { 0xffc6, Qt::Key_F9  },
    { 0xffc7, Qt::Key_F10 }, { 0xffc8, Qt::Key_F11 }, { 0xffc9, Qt::Key_F12 },
    { 0xffe1, Qt::Key_Shift    }, { 0xffe2, Qt::Key_Shift    },
    { 0xffe3, Qt::Key_Control  }, { 0xffe4, Qt::Key_Control  },
    { 0xffe5, Qt::Key_CapsLock },
    { 0xffe7, Qt::Key_Meta     }, { 0xffe8, Qt::Key_Meta     },
    { 0xffe9, Qt::Key_Alt      }, { 0xffea, Qt::Key_Alt      },
    { 0xffeb, Qt::Key_Meta     }, { 0xffec, Qt::Key_Meta     },
    { 0xff8d, Qt::Key_Enter    },
    { 0xff95, Qt::Key_Home     }, { 0xff96, Qt::Key_Left     },
    { 0xff97, Qt::Key_Up       }, { 0xff98, Qt::Key_Right    },
    { 0xff99, Qt::Key_Down     }, { 0xff9a, Qt::Key_PageUp   },
    { 0xff9b, Qt::Key_PageDown }, { 0xff9c, Qt::Key_End      },
    { 0xff9e, Qt::Key_Insert   }, { 0xff9f, Qt::Key_Delete   },
    { 0, 0 }
};

static int keysymToQtKey(quint32 sym)
{
    if (sym < 0x20)
        return 0;
    if (sym >= 0x20 && sym < 0x7f) {
        char c = (char)sym;
        if (c >= 'a' && c <= 'z') c -= 32;
        return (int)c;
    }
    for (int i = 0; SYM_QT[i].sym; i++) {
        if (SYM_QT[i].sym == sym)
            return SYM_QT[i].qtKey;
    }
    if (sym >= 0xffbe && sym <= 0xffc9)
        return Qt::Key_F1 + (sym - 0xffbe);
    return 0;
}

static Qt::KeyboardModifiers idkModsToQt(quint32 mods)
{
    Qt::KeyboardModifiers m = Qt::NoModifier;
    if (mods & IDK_MOD_CTRL)  m |= Qt::ControlModifier;
    if (mods & IDK_MOD_SHIFT) m |= Qt::ShiftModifier;
    if (mods & IDK_MOD_ALT)   m |= Qt::AltModifier;
    if (mods & IDK_MOD_SUPER) m |= Qt::MetaModifier;
    return m;
}

static quint64 nowMs() { return (quint64)QDateTime::currentMSecsSinceEpoch(); }

InputReceiver::InputReceiver(const QString &frameSocketPath, QObject *parent)
    : QObject(parent)
{
    m_socketPath = frameSocketPath + QStringLiteral("-input");
}

InputReceiver::~InputReceiver()
{
    stopRepeatTimer();
    closeFd();
}

bool InputReceiver::connectToInput()
{
    closeFd();

    memset(&m_tp, 0, sizeof(m_tp));
    if (idk_tp_init(&m_tp, IDK_TP_PRODUCER, m_socketPath.toUtf8().constData()) != 0) {
        IDK_LOG("input-rx", "Failed to init transport for %s\n", m_socketPath.toUtf8().data());
        return false;
    }

    if (!m_tp.ready) {
        IDK_LOG("input-rx", "Transport not ready for %s\n", m_socketPath.toUtf8().data());
        idk_tp_destroy(&m_tp);
        return false;
    }

    int watch_fd = m_tp._client_fd;
    if (m_tp.backend == IDK_TP_SHM) {
        m_wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (m_wakeFd < 0) {
            IDK_LOG("input-rx", "eventfd creation failed: %s\n", strerror(errno));
            idk_tp_destroy(&m_tp);
            return false;
        }
        watch_fd = m_wakeFd;

        struct RelayArgs {
            idk_transport_t *tp;
            int wakeFd;
        };
        auto *args = new RelayArgs{&m_tp, m_wakeFd};
        std::thread([args]() {
            void **shm_ptr_ptr = (void **)args->tp->_rsv;
            void *ptr = *shm_ptr_ptr;
            if (!ptr) { delete args; return; }

            std::atomic_int *slot_state = (std::atomic_int *)((char *)ptr + 80);

            while (true) {
                eventfd_t val;
                int err = eventfd_read(args->wakeFd, &val);
                if (err < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    break;

                int slot = slot_state->load();
                if (slot == 1) {
                    eventfd_write(args->wakeFd, 1);
                    usleep(1000);
                    continue;
                }

                struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
                syscall(__NR_futex, slot_state, FUTEX_WAIT, slot, &ts, NULL, 0);
            }
            delete args;
        }).detach();
    }

    m_notifier = new QSocketNotifier(watch_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated,
            this, &InputReceiver::onReadyRead);

    IDK_LOG("input-rx", "Connected to %s (backend=%s, fd=%d)\n",
            m_socketPath.toUtf8().data(),
            m_tp.backend == IDK_TP_SHM ? "shm" : "socket",
            watch_fd);
    return true;
}

void InputReceiver::disconnect()
{
    closeFd();
}

void InputReceiver::closeFd()
{
    if (m_notifier) {
        delete m_notifier;
        m_notifier = nullptr;
    }
    if (m_wakeFd >= 0) {
        ::close(m_wakeFd);
        m_wakeFd = -1;
    }
    if (m_tp.ready || m_tp._client_fd >= 0 || m_tp._server_fd >= 0) {
        idk_tp_destroy(&m_tp);
    }
    memset(&m_tp, 0, sizeof(m_tp));
    if (m_captureState) {
        m_captureState = false;
        emit inputCaptureChanged(false);
    }
}

QWidget *InputReceiver::focusProxy()
{
    if (!m_webview) return nullptr;
    return m_webview->focusProxy();
}

void InputReceiver::sendFocusIn()
{
    QWidget *fp = focusProxy();
    if (!fp) return;
    QFocusEvent fin(QEvent::FocusIn, Qt::OtherFocusReason);
    qApp->sendEvent(fp, &fin);
    fp->setFocus(Qt::OtherFocusReason);

    QMouseEvent mv(QEvent::MouseMove,
                   QPointF(m_mouseX, m_mouseY),
                   QPointF(m_mouseX, m_mouseY),
                   QPointF(m_mouseX, m_mouseY),
                   Qt::NoButton, m_buttons, Qt::NoModifier);
    mv.setTimestamp(nowMs());
    qApp->sendEvent(fp, &mv);

    m_focusSent = true;
}

void InputReceiver::onReadyRead()
{
    if (!m_tp.ready && m_tp._client_fd < 0 && m_wakeFd < 0) return;

    if (m_wakeFd >= 0) {
        eventfd_t val;
        while (eventfd_read(m_wakeFd, &val) == 0) { /* drain */ }
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
            if (nowCaptured) sendFocusIn();
        }

        if (nowCaptured && !m_focusSent)
            sendFocusIn();

        emit eventReceived(ev);

        switch (ev.type) {
        case IDK_INPUT_KEY:
            if (ev.u.key.keycode != 0)
                injectKeyboardEvent(ev);
            break;
        case IDK_INPUT_BUTTON:
        case IDK_INPUT_MOTION:
        case IDK_INPUT_AXIS:
            injectMouseEvent(ev);
            break;
        case IDK_INPUT_REPEAT:
            m_repeatRate = ev.u.repeat.rate > 0 ? ev.u.repeat.rate : 25;
            m_repeatDelay = ev.u.repeat.delay > 0 ? ev.u.repeat.delay : 500;
            IDK_LOG("input-rx", "repeat info: rate=%d cps delay=%d ms\n",
                    m_repeatRate, m_repeatDelay);
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

void InputReceiver::injectKeyboardEvent(const idk_input_event_t &ev)
{
    if (!m_webview) return;
    QWidget *fp = focusProxy();
    if (!fp) return;

    int qtKey = keysymToQtKey(ev.u.key.keysym);
    if (!qtKey) return;

    Qt::KeyboardModifiers mods = idkModsToQt(ev.mods);

    QString text;
    if (ev.u.key.keysym >= 0x20 && ev.u.key.keysym < 0x7f)
        text = QString(QChar((uint)ev.u.key.keysym));
    else if (ev.u.key.keysym == 0xff0d)
        text = QStringLiteral("\r");

    quint64 t = nowMs();
    bool isPress = (ev.flags & IDK_INPUT_FLAG_PRESS) != 0;

    if (isPress) {
        QKeyEvent p(QEvent::KeyPress, qtKey, mods, text, /*autorepeat=*/false);
        p.setTimestamp(t);
        qApp->sendEvent(fp, &p);

        if (ev.u.key.keycode != 0 && m_captureState)
            startRepeatTimer(ev.u.key.keycode, ev.u.key.keysym, ev.mods, text);
    } else {
        QKeyEvent r(QEvent::KeyRelease, qtKey, mods, text, /*autorepeat=*/false);
        r.setTimestamp(t);
        qApp->sendEvent(fp, &r);

        if (m_repeatKeycode == ev.u.key.keycode)
            stopRepeatTimer();
    }
}

void InputReceiver::startRepeatTimer(uint32_t keycode, uint32_t keysym,
                                      uint16_t mods, const QString &text)
{
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

void InputReceiver::stopRepeatTimer()
{
    if (m_repeatTimer)
        m_repeatTimer->stop();
    m_repeatKeycode = 0;
    m_repeatKeysym = 0;
    m_repeatArmed = false;
}

void InputReceiver::onRepeatTimeout()
{
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
    if (!qtKey) return;

    Qt::KeyboardModifiers mods = idkModsToQt(m_repeatMods);
    quint64 t = nowMs();

    QKeyEvent p(QEvent::KeyPress, qtKey, mods, m_repeatText, /*autorepeat=*/true);
    p.setTimestamp(t);
    qApp->sendEvent(fp, &p);

    if (m_repeatArmed) {
        m_repeatArmed = false;
        int interval = m_repeatRate > 0 ? (1000 / m_repeatRate) : 40;
        m_repeatTimer->setSingleShot(false);
        m_repeatTimer->start(interval);
    }
}

void InputReceiver::injectMouseEvent(const idk_input_event_t &ev)
{
    if (!m_webview) return;
    QWidget *fp = focusProxy();
    if (!fp) return;

    int x, y;
    if (ev.type == IDK_INPUT_BUTTON) {
        x = ev.u.btn.x;
        y = ev.u.btn.y;
    } else if (ev.type == IDK_INPUT_MOTION) {
        x = ev.u.motion.x;
        y = ev.u.motion.y;
    } else {
        x = m_mouseX;
        y = m_mouseY;
    }

    m_mouseX = x;
    m_mouseY = y;
    quint64 t = nowMs();
    QPointF local(x, y);

    switch (ev.type) {
    case IDK_INPUT_MOTION: {
        QMouseEvent mv(QEvent::MouseMove, local, local, local,
                       Qt::NoButton, m_buttons, Qt::NoModifier);
        mv.setTimestamp(t);
        qApp->sendEvent(fp, &mv);
        break;
    }
    case IDK_INPUT_BUTTON: {
        Qt::MouseButton sqBtn;
        if (ev.u.btn.button == 0x110)      sqBtn = Qt::LeftButton;
        else if (ev.u.btn.button == 0x111) sqBtn = Qt::RightButton;
        else if (ev.u.btn.button == 0x112) sqBtn = Qt::MiddleButton;
        else if (ev.u.btn.button == 0x113) sqBtn = Qt::XButton1;
        else if (ev.u.btn.button == 0x114) sqBtn = Qt::XButton2;
        else                          return;

        bool isPress = (ev.flags & IDK_INPUT_FLAG_PRESS) != 0;
        if (isPress) m_buttons |=  sqBtn;
        else         m_buttons &= ~sqBtn;

        QEvent::Type type = isPress ? QEvent::MouseButtonPress
                                    : QEvent::MouseButtonRelease;
        QMouseEvent be(type, local, local, local,
                       sqBtn, m_buttons, Qt::NoModifier);
        be.setTimestamp(t);
        qApp->sendEvent(fp, &be);
        break;
    }
    case IDK_INPUT_AXIS:
        injectWheelEvent(ev);
        break;
    }
}

void InputReceiver::injectWheelEvent(const idk_input_event_t &ev)
{
    if (!m_webview) return;
    QWidget *fp = focusProxy();
    if (!fp) return;

    QPointF local(m_mouseX, m_mouseY);

    const int scale = 12;
    int ax = -ev.u.axis.dx * scale;
    int ay = -ev.u.axis.dy * scale;

    auto post = [&](Qt::ScrollPhase phase, int dy, int dx = 0) {
        QWheelEvent e(local, local, QPoint(0, 0), QPoint(dx, dy),
                      m_buttons, Qt::NoModifier, phase, false);
        e.setTimestamp(nowMs());
        qApp->sendEvent(fp, &e);
    };

    post(Qt::ScrollBegin, 0);
    if (ax || ay) post(Qt::ScrollUpdate, ay, ax);
    post(Qt::ScrollEnd, 0);
}
