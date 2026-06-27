#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsfinae-incomplete"
#include <QUrl>
#include <QString>
#include <QStringList>
#include <QVariant>
#pragma GCC diagnostic pop

/**
 * Configuration for a single overlay group (webview).
 * Read from INI config file with [Section_Name] sections.
 *
 * Each section can have:
 *   - Match=<regex>   — process name regex to match (optional)
 *   - Url=<url>       — URL to render
 *   - Width=<px>      — initial width
 *   - Height=<px>     — initial height
 *   - X=<px>, Y=<px>  — screen position
 *
 * If any section has Match=, only sections whose Match regex matches
 * the current process name are loaded. Sections without Match= are
 * skipped when filtering is active.
 */
class GroupConfig
{
public:
    /* Construct from INI file + section name */
    explicit GroupConfig(const QString &confFile, const QString &group);

    /* Construct directly from CLI args (bypass INI file) */
    GroupConfig(const QUrl &url, int width, int height)
        : m_width(width), m_height(height), m_url(url) {}

    int width() const { return m_width; }
    int height() const { return m_height; }
    QUrl url() const { return m_url; }
    QString match() const { return m_match; }
    QStringList injectScripts() const { return m_injectScripts; }

private:
    QVariant value(const QString &key) const;

    QString m_confFile;
    QString m_group;

    int m_width = 640;
    int m_height = 480;
    QUrl m_url;
    QString m_match;  /* regex pattern for process name matching */
    QStringList m_injectScripts; /* list of JS file paths to inject */
};
