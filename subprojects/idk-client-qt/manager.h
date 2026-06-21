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

/**
 * Manager — manages socket connection to idk-overlay and webview lifecycle.
 *
 * Adapted from imgoverlay's Manager. Handles:
 * - Connecting to idk-overlay socket
 * - Managing WebView instances (one per overlay group)
 * - Sending frames via idk_client_* API
 */
class Manager : public QObject
{
    Q_OBJECT

public:
    /**
     * Create a new Manager.
     *
     * @param confFile  Path to INI config file
     * @param tray      Start minimized in system tray
     * @param parent    Parent QObject
     */
    explicit Manager(const QString &confFile, bool tray, QObject *parent = nullptr);
    ~Manager();

    /**
     * Check if connected to idk-overlay socket.
     */
    bool isConnected() const;

    /**
     * Get the resolved socket path from config.
     */
    QString socketPath() const { return m_socketPath; }

signals:
    /**
     * Emitted when socket connection is established.
     */
    void socketConnected();

    /**
     * Emitted when socket connection is lost.
     */
    void socketDisconnected();

private slots:
    /**
     * Handle incoming data from socket.
     */
    void onSocketReadyRead();

    /**
     * Reconnect timer — retry socket connection.
     */
    void onReconnectTimer();

private:
    /**
     * Initialize WebView instances from config.
     */
    void initWebViews();

    /**
     * Show/hide a specific view by tab index.
     */
    void showView(int index);

    /**
     * Update status label.
     */
    void updateStatus();

    /**
     * Resolve path relative to config file directory.
     */
    QString resolvePath(const QString &path) const;

    // State
    QSettings *m_settings;
    QString m_socketPath;
    QTimer *m_reconnectTimer;
    int m_disconnect_count = 0;  // throttle disconnect log spam
    bool m_was_connected = false; // track idk_client fd state transitions

    // UI
    QWidget *m_window;
    QTabBar *m_tabBar;
    QWidget *m_container;
    QLabel *m_statusLabel;
    QSystemTrayIcon *m_tray;

    // Overlays
    QList<WebView*> m_views;
};
