/* X11 input backend - event mask injection. */
#include "hook/x11_internal.h"

#define CAPTURE_EVENT_MASKS (KeyReleaseMask | ButtonReleaseMask | PointerMotionMask)

static pthread_mutex_t g_select_mutex = PTHREAD_MUTEX_INITIALIZER;

static int select_capture_masks(Display *dpy, Window window, long requested, bool preserve_current, bool *applied) {
  *applied = false;
  if (!orig_XSelectInput)
    orig_XSelectInput = (XSelectInput_fn)hook_orig("XSelectInput");
  if (!orig_XSelectInput || !dpy || !window)
    return 0;

  pthread_mutex_lock(&g_select_mutex);
  if (fn_XSync)
    fn_XSync(dpy, False);
  XWindowAttributesLayout attrs = {0};
  bool have_attrs = fn_XGetWindowAttributes && fn_XGetWindowAttributes(dpy, window, &attrs);
  if (preserve_current && !have_attrs) {
    pthread_mutex_unlock(&g_select_mutex);
    return 0;
  }

  long mask = requested | CAPTURE_EVENT_MASKS;
  if (preserve_current)
    mask |= attrs.your_event_mask;
  if (!(mask & ButtonPressMask) && have_attrs &&
      ((attrs.your_event_mask & ButtonPressMask) || !(attrs.all_event_masks & ButtonPressMask)))
    mask |= ButtonPressMask;

  int result = orig_XSelectInput(dpy, window, mask);
  if (fn_XSync)
    fn_XSync(dpy, False);
  pthread_mutex_unlock(&g_select_mutex);
  *applied = true;
  return result;
}

int hook_XSelectInput(Display *dpy, Window window, long mask) {
  bool applied;
  return select_capture_masks(dpy, window, mask, false, &applied);
}

void x11_ensure_event_masks(Display *dpy, Window window) {
  static _Thread_local Display *last_display;
  static _Thread_local Window last_window;
  if (!dpy || !window || (dpy == last_display && window == last_window))
    return;

  bool applied;
  select_capture_masks(dpy, window, NoEventMask, true, &applied);
  if (applied) {
    last_display = dpy;
    last_window = window;
  }
}
