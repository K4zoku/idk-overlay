#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>

#include "public/idk_ipc.h"  /* idk_input_event_t — same struct as wire protocol */

class QIODevice;
class QWebEngineView;
class QWidget;

class InputReceiver : public QObject
{
    Q_OBJECT

public:
    explicit InputReceiver(const QString &frameSocketPath, QObject *parent = nullptr);
    ~InputReceiver();

    bool connectToInput();
    void disconnect();
    bool isConnected() const { return m_fd >= 0; }

    void setWebView(QWebEngineView *view) { m_webview = view; m_focusProxy = nullptr; }

signals:
    void inputCaptureChanged(bool captured);
    void eventReceived(const idk_input_event_t &ev);

private slots:
    void onReadyRead();
    void onRepeatTimeout();

private:
    void closeFd();
    void injectKeyboardEvent(const idk_input_event_t &ev);
    void injectMouseEvent(const idk_input_event_t &ev);
    void injectWheelEvent(const idk_input_event_t &ev);
    QWidget *focusProxy();                  /* lazily resolved */
    void sendFocusIn();
    void startRepeatTimer(uint32_t keycode, uint32_t keysym, uint16_t mods,
                          const QString &text);
    void stopRepeatTimer();

    QString m_socketPath;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    bool m_captureState = false;
    QWebEngineView *m_webview = nullptr;
    QWidget *m_focusProxy = nullptr;

    int m_mouseX = 0;
    int m_mouseY = 0;
    Qt::MouseButtons m_buttons;
    bool m_focusSent = false;          /* Chromium focus has been pushed */

    /* Key repeat state — Wayland key repeat is client-side. When captured,
     * the game's SDL3 repeat timer never starts (we swallow key presses).
     * We implement repeat here using the rate/delay from wl_keyboard.repeat_info. */
    QTimer *m_repeatTimer = nullptr;
    int m_repeatRate = 25;             /* characters per second (default) */
    int m_repeatDelay = 500;           /* ms before first repeat (default) */
    bool m_repeatArmed = false;        /* true = initial delay, false = repeating */
    uint32_t m_repeatKeycode = 0;
    uint32_t m_repeatKeysym = 0;
    uint16_t m_repeatMods = 0;
    QString m_repeatText;
};
