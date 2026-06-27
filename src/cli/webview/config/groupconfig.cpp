#include "groupconfig.h"

#include <QFile>
#include <QSettings>
#include <QDir>

GroupConfig::GroupConfig(const QString &confFile, const QString &group)
    : m_confFile(confFile)
    , m_group(group)
{
    m_x = value("X").toInt();
    m_y = value("Y").toInt();
    m_width = value("Width").toInt();
    m_height = value("Height").toInt();
    m_url = value("Url").toUrl();
    m_match = value("Match").toString();

    QString scripts = value("InjectScripts").toString();
    if (!scripts.isEmpty()) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        const auto parts = scripts.split(',', Qt::SkipEmptyParts);
#else
        const auto parts = scripts.split(',', QString::SkipEmptyParts);
#endif
        QDir baseDir(QFileInfo(m_confFile).path());
        for (const QString &part : parts) {
            QString trimmed = part.trimmed();
            if (!trimmed.isEmpty()) {
                m_injectScripts.append(QFileInfo(trimmed).isAbsolute()
                    ? trimmed : baseDir.absoluteFilePath(trimmed));
            }
        }
    }
}

QVariant GroupConfig::value(const QString &key) const
{
    QSettings settings(m_confFile, QSettings::IniFormat);
    QString fullKey = QStringLiteral("%1/%2").arg(m_group, key);

    /* Exact match first */
    if (settings.contains(fullKey)) {
        return settings.value(fullKey);
    }

    /* Fallback: case-insensitive match against all keys in this group */
    settings.beginGroup(m_group);
    const auto subKeys = settings.childKeys();
    for (const QString &subKey : subKeys) {
        if (subKey.compare(key, Qt::CaseInsensitive) == 0) {
            return settings.value(subKey);
        }
    }
    settings.endGroup();

    return QVariant();
}
