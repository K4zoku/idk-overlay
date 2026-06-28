#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>

#include "public/idk_ipc.h"
#include "core/transport.h"

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
    bool isConnected() const { return m_tp.ready; }

    void setWebView(QWebEngineView *view) { m_webview = view; }

signals:
    void inputCaptureChanged(bool captured);
    void overlayVisibleChanged(bool visible);

private slots:
    void onReadyRead();
    void onRepeatTimeout();

private:
    void closeFd();
    QWidget *focusProxy();
    void sendFocusIn();
    void startRepeatTimer(uint32_t keycode, uint32_t keysym, uint16_t mods,
                          const QString &text);
    void stopRepeatTimer();

    QString m_socketPath;
    idk_transport_t m_tp;
    int m_wakeFd = -1;
    QSocketNotifier *m_notifier = nullptr;
    bool m_captureState = false;
    QWebEngineView *m_webview = nullptr;


    int m_mouseX = 0;
    int m_mouseY = 0;
    Qt::MouseButtons m_buttons;
    bool m_focusSent = false;

    QTimer *m_repeatTimer = nullptr;
    int m_repeatRate = 25;
    int m_repeatDelay = 500;
    bool m_repeatArmed = false;
    uint32_t m_repeatKeycode = 0;
    uint32_t m_repeatKeysym = 0;
    uint16_t m_repeatMods = 0;
    QString m_repeatText;
};
