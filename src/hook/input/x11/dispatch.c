/* X11 input backend - event dispatch. */
#include "hook/x11_internal.h"

Window g_game_window = 0;

/* Dispatch a single X event. Returns 1 if it should be swallowed
 * (captured or hotkey), 0 if it should be returned to the caller. */
int x11_dispatch_event(XEventStorage *ev) {
  if (!ev)
    return 0;
  int type = ev->xany.type;

  if (type >= KeyPress && type <= MotionNotify && ev->xany.display && ev->xany.window) {
    g_game_display = ev->xany.display;
    g_game_window = ev->xany.window;
  }

  switch (type) {
  case KeyPress:
  case KeyRelease:
    x11_ensure_event_masks(ev->xany.display, ev->xany.window);
    return x11_handle_key_event(ev);

  case ButtonPress:
  case ButtonRelease:
    return x11_handle_button_event(ev);

  case MotionNotify:
    return x11_handle_motion_event(ev);

  default:
    return 0;
  }
}

/* Fill ev with a harmless NoExpose event. */
void fill_noexpose(XEventStorage *ev, Display *dpy) {
  memset(ev, 0, sizeof(*ev));
  ev->xany.type = NoExpose;
  ev->xany.display = dpy;
}
