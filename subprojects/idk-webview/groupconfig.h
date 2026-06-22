#pragma once

#include <QUrl>
#include <QString>
#include <QVariant>

/**
 * Configuration for a single overlay group (webview).
 * Read from INI config file with [Overlay_N] sections.
 */
class GroupConfig
{
public:
    explicit GroupConfig(const QString &confFile, const QString &group);

    int x() const { return m_x; }
    int y() const { return m_y; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    QUrl url() const { return m_url; }

private:
    QVariant value(const QString &key) const;

    QString m_confFile;
    QString m_group;

    int m_x = 0;
    int m_y = 0;
    int m_width = 640;
    int m_height = 480;
    QUrl m_url;
};
