/* X11 input backend - display capture + capture/cursor state. */
#include "hook/x11_internal.h"

Display *g_game_display = NULL;
Cursor g_blank_cursor = 0;
Cursor g_saved_cursor = 0;
int g_cursor_grabbed = 0;

/* Capture toggle */

void idk_x11_input_set_capture(int enable) {
  int new_state = enable ? 1 : 0;
  if (new_state == g_captured)
    return;

  g_captured = new_state;
  XLOG("set_capture(%s)", new_state ? "ON" : "OFF");

  if (g_game_display && g_game_window && fn_XDefineCursor) {
    if (new_state && !g_blank_cursor && fn_XCreateFontCursor)
      g_blank_cursor = fn_XCreateFontCursor(g_game_display, 68);
    if (new_state && g_blank_cursor) {
      fn_XDefineCursor(g_game_display, g_game_window, g_blank_cursor);
      if (fn_XFlush)
        fn_XFlush(g_game_display);
    } else if (!new_state && g_blank_cursor && fn_XFreeCursor) {
      fn_XFreeCursor(g_game_display, g_blank_cursor);
      g_blank_cursor = 0;
    }
  }

  send_capture_state((uint32_t)new_state);
  if (new_state)
    send_repeat_info();
}

int idk_x11_input_is_captured(void) { return g_captured; }

/* Capture wine's X display as early as possible (winex11.drv calls
 * XOpenDisplay at init, before the game loads GL). Resolve the real
 * function from libX11 directly. */
typedef Display *(*XOpenDisplay_fn)(const char *);
static XOpenDisplay_fn orig_XOpenDisplay = NULL;

Display *XOpenDisplay(const char *name) {
  if (!orig_XOpenDisplay) {
    void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (!h)
      h = dlopen("libX11.so.6", RTLD_NOW);
    if (!h)
      h = dlopen("libX11.so", RTLD_NOW);
    if (h)
      orig_XOpenDisplay = (XOpenDisplay_fn)dlsym(h, "XOpenDisplay");
  }
  if (!orig_XOpenDisplay)
    return NULL;
  Display *dpy = orig_XOpenDisplay(name);
  if (dpy && !g_game_display)
    g_game_display = dpy;
  return dpy;
}

Display *idk_x11_game_display(void) { return g_game_display; }
