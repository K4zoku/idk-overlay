#include "manager.h"
#include "webview.h"

void Manager::onInputCaptureChanged(bool captured) {
  emit inputCaptureChanged(captured);

  if (m_views.isEmpty())
    return;

  QStringList events;
  if (captured && !m_lastCaptureState)
    events << QStringLiteral("new CustomEvent('overlaycapturestart')");
  else if (!captured && m_lastCaptureState)
    events << QStringLiteral("new CustomEvent('overlaycaptureend')");
  events << QStringLiteral("new CustomEvent('overlaycapturechanged',{detail:{captured:%1}})")
                .arg(captured ? "true" : "false");
  m_lastCaptureState = captured;

  QString js = QStringLiteral("(function(){"
                              "var evts=[%1];"
                              "for(var i=0;i<evts.length;i++)window.dispatchEvent(evts[i]);"
                              "})()")
                   .arg(events.join(","));
  for (auto *view : m_views) {
    if (view->page())
      view->page()->runJavaScript(js);
  }
}

void Manager::onOverlayVisibleChanged(bool visible) {
  emit overlayVisibleChanged(visible);

  if (m_views.isEmpty())
    return;

  QStringList events;
  if (visible && !m_lastVisibleState)
    events << QStringLiteral("new CustomEvent('overlayshow')");
  else if (!visible && m_lastVisibleState)
    events << QStringLiteral("new CustomEvent('overlayhide')");
  events << QStringLiteral("new CustomEvent('overlayvisiblechanged',{detail:{visible:%1}})")
                .arg(visible ? "true" : "false");
  m_lastVisibleState = visible;

  QString js = QStringLiteral("(function(){"
                              "var evts=[%1];"
                              "for(var i=0;i<evts.length;i++)window.dispatchEvent(evts[i]);"
                              "})()")
                   .arg(events.join(","));
  for (auto *view : m_views) {
    if (view->page())
      view->page()->runJavaScript(js);
  }
}
