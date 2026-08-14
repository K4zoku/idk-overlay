#include "groupconfig.h"
#include "manager.h"
#include "webview.h"

#include <QDebug>
#include <QRegularExpression>
#include <QSettings>

void Manager::initWebViews() {
  uint8_t i = 1;
  const auto groups = m_settings->childGroups();

  QString processName = m_cliMatch;
  if (processName.isEmpty()) {
    const char *envMatch = getenv("IDK_MATCH");
    if (envMatch && *envMatch)
      processName = QString::fromUtf8(envMatch);
  }

  if (!m_cliUrl.isEmpty()) {
    GroupConfig conf(m_cliUrl, m_cliWidth > 0 ? m_cliWidth : 1280, m_cliHeight > 0 ? m_cliHeight : 720);
    WebView *view = new WebView(i++, conf, this, m_noDmaBuf);
    view->setParent(m_container);
    view->show();
    m_views.append(view);
    m_tabBar->addTab("CLI");
    if (!m_views.isEmpty())
      showView(0);
    qDebug() << "Loaded 1 view from CLI args";
    return;
  }

  QRegularExpression re;
  bool hasMatchSections = false;
  if (!processName.isEmpty()) {
    for (const QString &group : groups) {
      QSettings s(m_settings->fileName(), QSettings::IniFormat);
      s.beginGroup(group);
      if (s.contains("Match")) {
        hasMatchSections = true;
        break;
      }
      s.endGroup();
    }
    if (hasMatchSections) {
      re.setPattern(processName);
    }
  }

  for (const QString &group : groups) {
    GroupConfig conf(m_settings->fileName(), group);

    if (hasMatchSections && re.isValid()) {
      QString matchPattern = conf.match();
      if (!matchPattern.isEmpty()) {
        QRegularExpression sectionRe(matchPattern);
        if (!sectionRe.isValid() || !sectionRe.match(processName).hasMatch()) {
          continue;
        }
      } else {
        continue;
      }
    }

    if (conf.url().isEmpty()) {
      qWarning() << "Invalid config" << group;
      continue;
    }
    WebView *view = new WebView(i++, conf, this, m_noDmaBuf);
    view->setParent(m_container);
    view->show();
    m_views.append(view);
    m_tabBar->addTab(group);
  }
  if (!m_views.isEmpty()) {
    showView(0);
  }
  qDebug() << "Loaded" << m_views.size() << "views"
           << (hasMatchSections ? QStringLiteral("(matched '%1')").arg(processName) : QString());
}

void Manager::showView(int index) {
  for (int i = 0; i < m_views.size(); ++i) {
    if (i == index) {
      m_views.at(i)->move(0, 0);
    } else {
      m_views.at(i)->move(9999, 9999);
    }
  }
}
