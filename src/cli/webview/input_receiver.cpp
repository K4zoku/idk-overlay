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
#include <cstring>
#include <cerrno>

#include "public/idk_ipc.h"
#include "core/log.h"

/* ─── XKB keysym → Qt::Key translation table ──────────────────────────── */

struct SymQtEntry { quint32 sym; int qtKey; };
static const SymQtEntry SYM_QT[] = {
    /* MISC function keys */
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
    /* Function keys */
    { 0xffbe, Qt::Key_F1  }, { 0xffbf, Qt::Key_F2  }, { 0xffc0, Qt::Key_F3  },
    { 0xffc1, Qt::Key_F4  }, { 0xffc2, Qt::Key_F5  }, { 0xffc3, Qt::Key_F6  },
    { 0xffc4, Qt::Key_F7  }, { 0xffc5, Qt::Key_F8  }, { 0xffc6, Qt::Key_F9  },
    { 0xffc7, Qt::Key_F10 }, { 0xffc8, Qt::Key_F11 }, { 0xffc9, Qt::Key_F12 },
    /* Modifier keys (Qt wants Key_Shift/Control/Alt/Meta even though text is empty) */
    { 0xffe1, Qt::Key_Shift    }, { 0xffe2, Qt::Key_Shift    },
    { 0xffe3, Qt::Key_Control  }, { 0xffe4, Qt::Key_Control  },
    { 0xffe5, Qt::Key_CapsLock },
    { 0xffe7, Qt::Key_Meta     }, { 0xffe8, Qt::Key_Meta     },
    { 0xffe9, Qt::Key_Alt      }, { 0xffea, Qt::Key_Alt      },
    { 0xffeb, Qt::Key_Meta     }, { 0xffec, Qt::Key_Meta     },
    /* Keypad equivalents */
    { 0xff8d, Qt::Key_Enter    },
    { 0xff95, Qt::Key_Home     }, { 0xff96, Qt::Key_Left     },
    { 0xff97, Qt::Key_Up       }, { 0xff98, Qt::Key_Right    },
    { 0xff99, Qt::Key_Down     }, { 0xff9a, Qt::Key_PageUp   },
    { 0xff9b, Qt::Key_PageDown }, { 0xff9c, Qt::Key_End      },
    { 0xff9e, Qt::Key_Insert   }, { 0xff9f, Qt::Key_Delete   },
    { 0, 0 }
};

/* Translate a single xkb keysym to a Qt::Key value.
 * For printable ASCII (0x20-0x7f) the Qt key is just the unicode
 * codepoint (lowercased for letters — Qt convention).
 * Returns 0 if the keysym cannot be translated. */
static int keysymToQtKey(quint32 sym)
{
    if (sym < 0x20)
        return 0;
    if (sym >= 0x20 && sym < 0x7f) {
        char c = (char)sym;
        if (c >= 'a' && c <= 'z') c -= 32;           /* Qt::Key_A = 0x41 (uppercase) */
        return (int)c;
    }
    for (int i = 0; SYM_QT[i].sym; i++) {
        if (SYM_QT[i].sym == sym)
            return SYM_QT[i].qtKey;
    }
    if (sym >= 0xffbe && sym <= 0xffc9)              /* F1-F12 fallback */
        return Qt::Key_F1 + (sym - 0xffbe);
    return 0;
}

/* Translate the idk modifier bitmask to Qt::KeyboardModifiers. */
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

/* ─── InputReceiver ─────────────────────────────────────────────────────── */

InputReceiver::InputReceiver(const QString &frameSocketPath, QObject *parent)
    : QObject(parent)
{
    m_socketPath = frameSocketPath + QStringLiteral("-input");
}

InputReceiver::~InputReceiver()
{
    closeFd();
}

bool InputReceiver::connectToInput()
{
    closeFd();

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    QByteArray pathBytes = m_socketPath.toUtf8();
    if (pathBytes.size() >= (int)sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::memcpy(addr.sun_path, pathBytes.constData(), pathBytes.size());

    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    m_fd = fd;

    int fl = fcntl(m_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(m_fd, F_SETFL, fl | O_NONBLOCK);

    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated,
            this, &InputReceiver::onReadyRead);

    IDK_LOG("input-rx", "Connected to %s (fd=%d)\n",
            m_socketPath.toUtf8().data(), m_fd);
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
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_captureState) {
        m_captureState = false;
        emit inputCaptureChanged(false);
    }
}

QWidget *InputReceiver::focusProxy()
{
    if (!m_webview) return nullptr;
    if (m_focusProxy) return m_focusProxy;
    /* focusProxy is created lazily after the page finishes loading */
    m_focusProxy = m_webview->focusProxy();
    return m_focusProxy;
}

void InputReceiver::sendFocusIn()
{
    QWidget *fp = focusProxy();
    if (!fp) return;
    QFocusEvent fin(QEvent::FocusIn, Qt::OtherFocusReason);
    qApp->sendEvent(fp, &fin);
    fp->setFocus(Qt::OtherFocusReason);

    /* Synthesize one MouseMove so Chromium's RenderWidgetHost has a valid
     * previous mouse position — some sites won't hit-test correctly
     * otherwise. We rely on the next real mouse event to take over. */
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
    if (m_fd < 0) return;

    idk_input_event_t ev;
    while (true) {
        ssize_t n = ::read(m_fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev)) {
            /* No magic/checksum in new protocol — just validate type range. */
            if (ev.type < IDK_INPUT_KEY || ev.type > IDK_INPUT_REPEAT) {
                closeFd();
                return;
            }

            InputEvent ie;
            ie.type    = ev.type;
            ie.flags   = ev.flags;
            ie.mods    = ev.mods;
            ie.time    = ev.time;
            /* Extract union payload based on type */
            ie.keycode = ev.u.key.keycode;
            ie.keysym  = ev.u.key.keysym;
            ie.button  = ev.u.btn.button;
            ie.x       = ev.u.motion.x;
            ie.y       = ev.u.motion.y;
            ie.dx      = ev.u.axis.dx;
            ie.dy      = ev.u.axis.dy;
            ie.rate    = ev.u.repeat.rate;
            ie.delay   = ev.u.repeat.delay;

            bool nowCaptured = (ev.flags & IDK_INPUT_FLAG_CAPTURE) != 0;
            if (nowCaptured != m_captureState) {
                m_captureState = nowCaptured;
                m_focusSent = false;
                /* Stop key repeat when capture goes OFF */
                if (!nowCaptured)
                    stopRepeatTimer();
                emit inputCaptureChanged(nowCaptured);
                IDK_LOG("input-rx", "capture %s\n", nowCaptured ? "ENABLED" : "DISABLED");
                /* When capture goes ON, give Chromium focus so subsequent
                 * key events reach the page. (focusProxy may not be ready
                 * yet if the page is still loading — we retry below.) */
                if (nowCaptured) sendFocusIn();
            }

            /* Lazy retry: if page finished loading after capture-on, push
             * focus as soon as focusProxy becomes available. */
            if (nowCaptured && !m_focusSent)
                sendFocusIn();

            emit eventReceived(ie);

            switch (ev.type) {
            case IDK_INPUT_KEY:
                if (ie.keycode != 0)
                    injectKeyboardEvent(ie);
                break;
            case IDK_INPUT_BUTTON:
            case IDK_INPUT_MOTION:
            case IDK_INPUT_AXIS:
                injectMouseEvent(ie);
                break;
            case IDK_INPUT_REPEAT:
                /* wl_keyboard.repeat_info from compositor:
                 * rate = cps, delay = ms before first repeat */
                m_repeatRate = ie.rate > 0 ? ie.rate : 25;
                m_repeatDelay = ie.delay > 0 ? ie.delay : 500;
                IDK_LOG("input-rx", "repeat info: rate=%d cps delay=%d ms\n",
                        m_repeatRate, m_repeatDelay);
                break;
            }
        } else if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            closeFd();
            return;
        } else if (n == 0) {
            closeFd();
            return;
        } else {
            closeFd();
            return;
        }
    }
}

/* ─── Keyboard ─────────────────────────────────────────────────────────── */

void InputReceiver::injectKeyboardEvent(const InputEvent &ev)
{
    if (!m_webview) return;
    QWidget *fp = focusProxy();
    if (!fp) return;

    int qtKey = keysymToQtKey(ev.keysym);
    if (!qtKey) return;

    Qt::KeyboardModifiers mods = idkModsToQt(ev.mods);

    /* For printable ASCII, attach the character as text so Chromium inserts
     * it into the focused input field. The keysym from xkb already reflects
     * the current shift/altgr state (uppercase, etc). */
    QString text;
    if (ev.keysym >= 0x20 && ev.keysym < 0x7f)
        text = QString(QChar((uint)ev.keysym));
    else if (ev.keysym == 0xff0d)
        text = QStringLiteral("\r");

    quint64 t = nowMs();
    bool isPress = (ev.flags & IDK_INPUT_FLAG_PRESS) != 0;

    if (isPress) {
        QKeyEvent p(QEvent::KeyPress, qtKey, mods, text, /*autorepeat=*/false);
        p.setTimestamp(t);
        qApp->sendEvent(fp, &p);

        /* Start repeat timer for this key (unless it's a modifier or
         * other non-repeatable key). Wayland repeat is client-side,
         * and since we swallowed the press from the game, the game's
         * SDL3 repeat timer never starts — we must do it ourselves. */
        if (ev.keycode != 0 && m_captureState)
            startRepeatTimer(ev.keycode, ev.keysym, ev.mods, text);
    } else {
        QKeyEvent r(QEvent::KeyRelease, qtKey, mods, text, /*autorepeat=*/false);
        r.setTimestamp(t);
        qApp->sendEvent(fp, &r);

        /* Stop any active repeat for this key */
        if (m_repeatKeycode == ev.keycode)
            stopRepeatTimer();
    }
}

/* ─── Key repeat ───────────────────────────────────────────────────────── */

void InputReceiver::startRepeatTimer(uint32_t keycode, uint32_t keysym,
                                      uint32_t mods, const QString &text)
{
    if (!m_repeatTimer) {
        m_repeatTimer = new QTimer(this);
        m_repeatTimer->setSingleShot(true);
        connect(m_repeatTimer, &QTimer::timeout, this, &InputReceiver::onRepeatTimeout);
    }

    /* If a different key was repeating, stop it first */
    if (m_repeatKeycode != 0 && m_repeatKeycode != keycode)
        stopRepeatTimer();

    m_repeatKeycode = keycode;
    m_repeatKeysym = keysym;
    m_repeatMods = mods;
    m_repeatText = text;
    m_repeatArmed = true;  /* first tick = initial delay, then switch to repeat interval */

    /* Initial delay before first repeat */
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

    /* Send autorepeat KeyPress */
    QKeyEvent p(QEvent::KeyPress, qtKey, mods, m_repeatText, /*autorepeat=*/true);
    p.setTimestamp(t);
    qApp->sendEvent(fp, &p);

    if (m_repeatArmed) {
        /* First repeat fired — switch to repeat interval (1000/rate ms) */
        m_repeatArmed = false;
        int interval = m_repeatRate > 0 ? (1000 / m_repeatRate) : 40;
        m_repeatTimer->setSingleShot(false);
        m_repeatTimer->start(interval);
    }
    /* Subsequent ticks: QTimer fires repeatedly at repeat interval */
}

/* ─── Mouse ─────────────────────────────────────────────────────────────── */

void InputReceiver::injectMouseEvent(const InputEvent &ev)
{
    if (!m_webview) return;
    QWidget *fp = focusProxy();
    if (!fp) return;

    m_mouseX = ev.x;
    m_mouseY = ev.y;
    quint64 t = nowMs();
    QPointF local(ev.x, ev.y);

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
        if (ev.button == 0x110)      sqBtn = Qt::LeftButton;
        else if (ev.button == 0x111) sqBtn = Qt::RightButton;
        else if (ev.button == 0x112) sqBtn = Qt::MiddleButton;
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

/* ─── Wheel ─────────────────────────────────────────────────────────────── */

/* Wayland axis events deliver raw pixel deltas per frame. Chromium treats
 * wheel input through a phase-aware path; a lone QWheelEvent with
 * Qt::NoScrollPhase does NOT trigger the default scroll action. We therefore
 * emit a Begin → Update → End burst for every axis event so the renderer's
 * input router commits the scroll consistently. */
void InputReceiver::injectWheelEvent(const InputEvent &ev)
{
    if (!m_webview) return;
    QWidget *fp = focusProxy();
    if (!fp) return;

    QPointF local(m_mouseX, m_mouseY);

    /* Convert wayland axis units (~10 per wheel notch on most compositors)
     * to Qt angle ticks (120 per notch). Sign matches the JS path we are
     * replacing: scrollBy(0, dy * 20) which scrolls down for positive dy.
     * angleDelta positive = scroll up, so negate. */
    const int scale = 12;
    int ax = -ev.dx * scale;
    int ay = -ev.dy * scale;

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