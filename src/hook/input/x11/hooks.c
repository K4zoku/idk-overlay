/* X11 input backend - XNextEvent-family hook wrappers.
 *
 * Generic wrapper: call orig, if it returned an event, dispatch it.
 * If dispatch says "swallow", loop to get the next event (for blocking
 * variants). For non-blocking (XCheck*) variants, return 0 to indicate
 * "no event available".
 */
#include "hook/x11_internal.h"

#define X11_DEFINE_ORIG(ret, name, params) name##_fn orig_##name = NULL;
X11_EVENT_FOREACH(X11_DEFINE_ORIG)
#undef X11_DEFINE_ORIG

int hook_XNextEvent(Display *dpy, XEventStorage *ev) {
  if (!orig_XNextEvent)
    orig_XNextEvent = (XNextEvent_fn)hook_orig("XNextEvent");
  if (!g_game_display)
    g_game_display = dpy;

  int r = orig_XNextEvent(dpy, ev);
  if (r != 0)
    return r;
  if (x11_dispatch_event(ev)) {
    fill_noexpose(ev, dpy);
  }
  return 0;
}

int hook_XPeekEvent(Display *dpy, XEventStorage *ev) {
  if (!orig_XPeekEvent)
    orig_XPeekEvent = (XPeekEvent_fn)hook_orig("XPeekEvent");
  if (!g_game_display)
    g_game_display = dpy;

  int r = orig_XPeekEvent(dpy, ev);
  if (r != 0)
    return r;
  x11_dispatch_event(ev);
  return 0;
}

int hook_XCheckWindowEvent(Display *dpy, Window w, long mask, XEventStorage *ev) {
  if (!orig_XCheckWindowEvent)
    orig_XCheckWindowEvent = (XCheckWindowEvent_fn)hook_orig("XCheckWindowEvent");
  if (!g_game_display)
    g_game_display = dpy;
  if (!g_game_window && w)
    g_game_window = w;

  int r = orig_XCheckWindowEvent(dpy, w, mask, ev);
  if (r == 0)
    return 0;
  if (x11_dispatch_event(ev)) {
    fill_noexpose(ev, dpy);
  }
  return 1;
}

int hook_XMaskEvent(Display *dpy, long mask, XEventStorage *ev) {
  if (!orig_XMaskEvent)
    orig_XMaskEvent = (XMaskEvent_fn)hook_orig("XMaskEvent");
  if (!g_game_display)
    g_game_display = dpy;

  int r = orig_XMaskEvent(dpy, mask, ev);
  if (r != 0)
    return r;
  if (x11_dispatch_event(ev)) {
    fill_noexpose(ev, dpy);
  }
  return 0;
}

int hook_XCheckMaskEvent(Display *dpy, long mask, XEventStorage *ev) {
  if (!orig_XCheckMaskEvent)
    orig_XCheckMaskEvent = (XCheckMaskEvent_fn)hook_orig("XCheckMaskEvent");
  if (!g_game_display)
    g_game_display = dpy;

  int r = orig_XCheckMaskEvent(dpy, mask, ev);
  if (r == 0)
    return 0;
  if (x11_dispatch_event(ev)) {
    fill_noexpose(ev, dpy);
  }
  return 1;
}

int hook_XCheckTypedEvent(Display *dpy, int type, XEventStorage *ev) {
  if (!orig_XCheckTypedEvent)
    orig_XCheckTypedEvent = (XCheckTypedEvent_fn)hook_orig("XCheckTypedEvent");
  if (!g_game_display)
    g_game_display = dpy;

  int r = orig_XCheckTypedEvent(dpy, type, ev);
  if (r == 0)
    return 0;
  if (x11_dispatch_event(ev)) {
    fill_noexpose(ev, dpy);
  }
  return 1;
}

int hook_XCheckTypedWindowEvent(Display *dpy, Window w, int type, XEventStorage *ev) {
  if (!orig_XCheckTypedWindowEvent)
    orig_XCheckTypedWindowEvent = (XCheckTypedWindowEvent_fn)hook_orig("XCheckTypedWindowEvent");
  if (!g_game_display)
    g_game_display = dpy;
  if (!g_game_window && w)
    g_game_window = w;

  int r = orig_XCheckTypedWindowEvent(dpy, w, type, ev);
  if (r == 0)
    return 0;
  if (x11_dispatch_event(ev)) {
    fill_noexpose(ev, dpy);
  }
  return 1;
}

int hook_XWindowEvent(Display *dpy, Window w, long mask, XEventStorage *ev) {
  if (!orig_XWindowEvent)
    orig_XWindowEvent = (XWindowEvent_fn)hook_orig("XWindowEvent");
  if (!g_game_display)
    g_game_display = dpy;
  if (!g_game_window && w)
    g_game_window = w;

  int r = orig_XWindowEvent(dpy, w, mask, ev);
  if (r != 0)
    return r;
  if (x11_dispatch_event(ev)) {
    fill_noexpose(ev, dpy);
  }
  return 0;
}

int hook_XIfEvent(Display *dpy, XEventStorage *ev, void *pred, void *arg) {
  if (!orig_XIfEvent)
    orig_XIfEvent = (XIfEvent_fn)hook_orig("XIfEvent");
  if (!g_game_display)
    g_game_display = dpy;

  int r = orig_XIfEvent(dpy, ev, pred, arg);
  if (r != 0)
    return r;
  if (x11_dispatch_event(ev)) {
    fill_noexpose(ev, dpy);
  }
  return 0;
}

int hook_XCheckIfEvent(Display *dpy, XEventStorage *ev, void *pred, void *arg) {
  if (!orig_XCheckIfEvent)
    orig_XCheckIfEvent = (XCheckIfEvent_fn)hook_orig("XCheckIfEvent");
  if (!g_game_display)
    g_game_display = dpy;

  int r = orig_XCheckIfEvent(dpy, ev, pred, arg);
  if (r == 0)
    return 0;
  if (x11_dispatch_event(ev)) {
    fill_noexpose(ev, dpy);
  }
  return 1;
}
