/* X11 input backend - event mask injection (select + retroactive). */
#include "hook/x11_internal.h"

/* XSelectInput hook - inject pointer event masks so we receive mouse events
 * even if the game didn't request them (e.g. glxgears only selects KeyPress).
 * We OR-in ButtonPressMask | ButtonReleaseMask | PointerMotionMask so that
 * ButtonPress/ButtonRelease/MotionNotify events flow into the X event queue
 * where our XNextEvent-family hooks can intercept them when captured. */
int hook_XSelectInput(Display *dpy, Window w, long mask) {
  if (!orig_XSelectInput)
    orig_XSelectInput = (XSelectInput_fn)hook_orig("XSelectInput");
  if (!g_game_display)
    g_game_display = dpy;
  if (!g_game_window && w)
    g_game_window = w;

  mask |= ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyReleaseMask;

  return orig_XSelectInput(dpy, w, mask);
}

/* Retroactively inject pointer + key release masks on the game window.
 * Done once per process (g_masks_injected flag). */
void x11_retroactive_masks(void) {
  static int g_masks_injected = 0;
  if (!g_masks_injected && g_game_display && g_game_window && orig_XSelectInput) {
    g_masks_injected = 1;
    long extra = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyReleaseMask;
    orig_XSelectInput(g_game_display, g_game_window, extra);
    XLOG("retroactively set event masks: 0x%lx on window 0x%lx", extra, (unsigned long)g_game_window);
  }
}
