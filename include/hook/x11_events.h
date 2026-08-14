#ifndef IDK_X11_EVENTS_H
#define IDK_X11_EVENTS_H

#include "hook/x11_layouts.h"

/* X11 function pointers — X-macro pattern */

#define X11_EVENT_FOREACH(F)                                                                                           \
  F(int, XNextEvent, (Display *, XEventStorage *))                                                                     \
  F(int, XPeekEvent, (Display *, XEventStorage *))                                                                     \
  F(int, XCheckWindowEvent, (Display *, Window, long, XEventStorage *))                                                \
  F(int, XMaskEvent, (Display *, long, XEventStorage *))                                                               \
  F(int, XCheckMaskEvent, (Display *, long, XEventStorage *))                                                          \
  F(int, XCheckTypedEvent, (Display *, int, XEventStorage *))                                                          \
  F(int, XCheckTypedWindowEvent, (Display *, Window, int, XEventStorage *))                                            \
  F(int, XWindowEvent, (Display *, Window, long, XEventStorage *))                                                     \
  F(int, XIfEvent, (Display *, XEventStorage *, void *, void *))                                                       \
  F(int, XCheckIfEvent, (Display *, XEventStorage *, void *, void *))                                                  \
  F(int, XPending, (Display *))                                                                                        \
  F(int, XEventsQueued, (Display *, int))                                                                              \
  F(int, XSelectInput, (Display *, Window, long))

#define X11_CURSOR_FOREACH(F)                                                                                          \
  F(int, XGetWindowAttributes, (Display *, Window, void *))                                                            \
  F(Cursor, XCreatePixmapCursor, (Display *, void *, void *, void *, void *, unsigned int, unsigned int))              \
  F(Cursor, XCreateFontCursor, (Display *, unsigned int))                                                              \
  F(int, XFreePixmap, (Display *, void *))                                                                             \
  F(void, XDefineCursor, (Display *, Window, Cursor))                                                                  \
  F(int, XUndefineCursor, (Display *, Window))                                                                         \
  F(int, XFreeCursor, (Display *, Cursor))                                                                             \
  F(int, XGrabPointer, (Display *, Window, Bool, unsigned int, int, int, Window, Cursor, Time))                        \
  F(int, XUngrabPointer, (Display *, Time))                                                                            \
  F(KeySym, XKeycodeToKeysym, (Display *, KeyCode, int))                                                               \
  F(KeySym, XStringToKeysym, (const char *))                                                                           \
  F(int, XFlush, (Display *))                                                                                          \
  F(int, XSync, (Display *, Bool))

#define X11_TYPEDEF(ret, name, params) typedef ret(*name##_fn) params;
X11_EVENT_FOREACH(X11_TYPEDEF)
X11_CURSOR_FOREACH(X11_TYPEDEF)
#undef X11_TYPEDEF

#define X11_EXTERN_ORIG(ret, name, params) extern name##_fn orig_##name;
#define X11_EXTERN_FN(ret, name, params) extern name##_fn fn_##name;
X11_EVENT_FOREACH(X11_EXTERN_ORIG)
X11_CURSOR_FOREACH(X11_EXTERN_FN)
#undef X11_EXTERN_ORIG
#undef X11_EXTERN_FN

#endif /* IDK_X11_EVENTS_H */
