#pragma once

#include <QObject>
#include <QTimer>
#include <QSettings>
#include <QString>
#include <QList>
#include <QWidget>
#include <QTabBar>
#include <QLabel>
#include <QSystemTrayIcon>

class WebView;
class InputReceiver;

/**
 * Manager — manages socket connection to idk-overlay and webview lifecycle.
 *
 * Adapted from imgoverlay's Manager. Handles:
 * - Connecting to idk-overlay socket
 * - Managing WebView instances (one per overlay group)
 * - Sending frames via idk_fs_* API
 * - Receiving input events from the game's wayland input hook
 *   (InputReceiver, when the user toggles input capture with F8)
 */
class Manager : public QObject
{
    Q_OBJECT

public:
    explicit Manager(const QString &confFile,
                     const QString &socketPath,
                     bool noDmaBuf = false,
                     const QString &cliUrl = QString(),
                     int cliWidth = 0,
                     int cliHeight = 0,
                     const QString &cliMatch = QString(),
                     QObject *parent = nullptr);
    ~Manager();

    bool isConnected() const;
    QString socketPath() const { return m_socketPath; }

signals:
    void socketConnected();
    void socketDisconnected();
    void inputCaptureChanged(bool captured);
    void overlayVisibleChanged(bool visible);

private slots:
    void onInputCaptureChanged(bool captured);
    void onOverlayVisibleChanged(bool visible);

private:
    void initWebViews();
    void showView(int index);
    void updateStatus();
    QString resolvePath(const QString &path) const;
    void startInputReceiver();
    void stopInputReceiver();

    // State
    QSettings *m_settings;
    QString m_socketPath;
    QTimer *m_reconnectTimer;
    int m_disconnect_count = 0;  // throttle disconnect log spam
    bool m_was_connected = false; // track idk_fs fd state transitions
    bool m_noDmaBuf = false;     // force SHM mode
    QString m_cliUrl;            // CLI --url override
    int m_cliWidth = 0;          // CLI --width override
    int m_cliHeight = 0;         // CLI --height override
    QString m_cliMatch;          // CLI --match (process name regex)
    InputReceiver *m_inputRx = nullptr;
    QTimer *m_inputRetryTimer = nullptr;
    bool m_lastVisibleState = false;
    bool m_lastCaptureState = false;

    // UI
    QWidget *m_window;
    QTabBar *m_tabBar;
    QWidget *m_container;
    QLabel *m_statusLabel;
    QSystemTrayIcon *m_tray;

    // Window geometry (for tray toggle show/hide)
    int m_lastX = 0, m_lastY = 0, m_lastW = 800, m_lastH = 600;

    // Overlays
    QList<WebView*> m_views;
};
