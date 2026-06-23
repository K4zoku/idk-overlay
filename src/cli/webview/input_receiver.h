#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QString>

class QIODevice;
class QWebEngineView;

class InputReceiver : public QObject
{
    Q_OBJECT

public:
    explicit InputReceiver(const QString &frameSocketPath, QObject *parent = nullptr);
    ~InputReceiver();

    bool connectToInput();
    void disconnect();
    bool isConnected() const { return m_fd >= 0; }

    void setWebView(QWebEngineView *view) { m_webview = view; }

signals:
    void inputCaptureChanged(bool captured);
    void cursorMoved(int x, int y);
    void eventReceived(const struct InputEvent &ev);

private slots:
    void onReadyRead();

private:
    void closeFd();
    void injectKeyboardEvent(const struct InputEvent &ev);
    void injectMouseEvent(const struct InputEvent &ev);

    QString m_socketPath;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    bool m_captureState = false;
    QWebEngineView *m_webview = nullptr;

    int m_mouseX = 0;
    int m_mouseY = 0;
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
