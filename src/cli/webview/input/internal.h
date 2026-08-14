#ifndef IDK_WEBVIEW_INPUT_INTERNAL_H
#define IDK_WEBVIEW_INPUT_INTERNAL_H

#include <QDateTime>
#include <Qt>

/* Keysym translation + timing helpers shared across the input/ split.
 * Defined in keys.cpp. Not part of the public symbol surface. */
int keysymToQtKey(quint32 sym);
Qt::KeyboardModifiers idkModsToQt(quint32 mods);
quint64 nowMs();

#endif /* IDK_WEBVIEW_INPUT_INTERNAL_H */
