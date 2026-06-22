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
 * - Sending frames via idk_fs_* API
 */
class Manager : public QObject
{
    Q_OBJECT

public:
    explicit Manager(const QString &confFile, bool tray, QObject *parent = nullptr);
    ~Manager();

    bool isConnected() const;
    QString socketPath() const { return m_socketPath; }

signals:
    void socketConnected();
    void socketDisconnected();

private slots:
    void onSocketReadyRead();
    void onReconnectTimer();

private:
    void initWebViews();
    void showView(int index);
    void updateStatus();
    QString resolvePath(const QString &path) const;

    // State
    QSettings *m_settings;
    QString m_socketPath;
    QTimer *m_reconnectTimer;
    int m_disconnect_count = 0;  // throttle disconnect log spam
    bool m_was_connected = false; // track idk_fs fd state transitions

    // UI
    QWidget *m_window;
    QTabBar *m_tabBar;
    QWidget *m_container;
    QLabel *m_statusLabel;
    QSystemTrayIcon *m_tray;

    // Overlays
    QList<WebView*> m_views;
};
