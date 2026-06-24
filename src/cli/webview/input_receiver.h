#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QString>

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
    void eventReceived(const struct InputEvent &ev);

private slots:
    void onReadyRead();

private:
    void closeFd();
    void injectKeyboardEvent(const struct InputEvent &ev);
    void injectMouseEvent(const struct InputEvent &ev);
    void injectWheelEvent(const struct InputEvent &ev);
    QWidget *focusProxy();                  /* lazily resolved */
    void sendFocusIn();

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
};

struct InputEvent {
    quint32 type;
    quint32 time;
    quint32 serial;
    quint32 keycode;
    quint32 keysym;
    quint32 state;
    quint32 button;
    qint32  x;
    qint32  y;
    qint32  dx;
    qint32  dy;
    quint32 mods;
    quint32 capture;
};