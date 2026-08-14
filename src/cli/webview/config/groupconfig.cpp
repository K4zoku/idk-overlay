#include "groupconfig.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSettings>

#include "core/log.h"

GroupConfig::GroupConfig(const QString &confFile, const QString &group) : m_confFile(confFile), m_group(group) {
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
        QString resolved = QFileInfo(trimmed).isAbsolute() ? trimmed : baseDir.absoluteFilePath(trimmed);
        m_injectScripts.append(resolved);
        IDK_LOG("webview", "InjectScripts: '%s' → '%s'\n", trimmed.toUtf8().data(), resolved.toUtf8().data());
      }
    }
  }
}

QVariant GroupConfig::value(const QString &key) const {
  QSettings settings(m_confFile, QSettings::IniFormat);
  QString fullKey = QStringLiteral("%1/%2").arg(m_group, key);

  if (settings.contains(fullKey)) {
    return settings.value(fullKey);
  }

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
