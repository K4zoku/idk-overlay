/* X11 input backend - display capture + capture/cursor state. */
#include "hook/input_backend.h"
#include "hook/x11_internal.h"

Display *g_game_display = NULL;
Cursor g_overlay_cursor = 0;
static uint32_t g_applied_cursor_generation = 0;
static void *g_xcursor_handle = NULL;

typedef uint32_t XcursorPixel;
typedef struct {
  unsigned int version;
  unsigned int size;
  unsigned int width;
  unsigned int height;
  unsigned int xhot;
  unsigned int yhot;
  unsigned int delay;
  XcursorPixel *pixels;
} XcursorImage;

static XcursorImage *(*fn_XcursorImageCreate)(int, int);
static void (*fn_XcursorImageDestroy)(XcursorImage *);
static Cursor (*fn_XcursorImageLoadCursor)(Display *, const XcursorImage *);
static Cursor (*fn_XcursorLibraryLoadCursor)(Display *, const char *);

static int resolve_xcursor(void) {
  if (g_xcursor_handle)
    return 0;
  g_xcursor_handle = dlopen("libXcursor.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!g_xcursor_handle)
    return -1;
  fn_XcursorImageCreate = dlsym(g_xcursor_handle, "XcursorImageCreate");
  fn_XcursorImageDestroy = dlsym(g_xcursor_handle, "XcursorImageDestroy");
  fn_XcursorImageLoadCursor = dlsym(g_xcursor_handle, "XcursorImageLoadCursor");
  fn_XcursorLibraryLoadCursor = dlsym(g_xcursor_handle, "XcursorLibraryLoadCursor");
  if (!fn_XcursorImageCreate || !fn_XcursorImageDestroy || !fn_XcursorImageLoadCursor || !fn_XcursorLibraryLoadCursor) {
    dlclose(g_xcursor_handle);
    g_xcursor_handle = NULL;
    return -1;
  }
  return 0;
}

static Cursor create_custom_cursor(const idk_cursor_update_t *cursor, const uint8_t *pixels) {
  if (resolve_xcursor() != 0)
    return 0;
  unsigned int width = cursor->visible ? cursor->width : 1;
  unsigned int height = cursor->visible ? cursor->height : 1;
  XcursorImage *image = fn_XcursorImageCreate((int)width, (int)height);
  if (!image)
    return 0;
  image->xhot = cursor->visible && cursor->hotspot_x > 0 ? (unsigned int)cursor->hotspot_x : 0;
  image->yhot = cursor->visible && cursor->hotspot_y > 0 ? (unsigned int)cursor->hotspot_y : 0;
  if (image->xhot >= width)
    image->xhot = width - 1;
  if (image->yhot >= height)
    image->yhot = height - 1;
  if (cursor->visible) {
    size_t count = (size_t)width * height;
    for (size_t i = 0; i < count; i++) {
      const uint8_t *p = pixels + i * 4;
      image->pixels[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
    }
  } else {
    image->pixels[0] = 0;
  }
  Cursor result = fn_XcursorImageLoadCursor(g_game_display, image);
  fn_XcursorImageDestroy(image);
  return result;
}

static const char *cursor_name(uint8_t shape) {
  static const char *const names[] = {
      NULL,         "default",  "context-menu",  "help",      "pointer",     "progress",    "wait",       "cell",
      "crosshair",  "text",     "vertical-text", "alias",     "copy",        "move",        "no-drop",    "not-allowed",
      "grab",       "grabbing", "e-resize",      "n-resize",  "ne-resize",   "nw-resize",   "s-resize",   "se-resize",
      "sw-resize",  "w-resize", "ew-resize",     "ns-resize", "nesw-resize", "nwse-resize", "col-resize", "row-resize",
      "all-scroll", "zoom-in",  "zoom-out",      "dnd-ask",   "all-resize",
  };
  return shape <= IDK_CURSOR_ALL_RESIZE ? names[shape] : NULL;
}

static void replace_cursor(Cursor cursor) {
  if (cursor && fn_XDefineCursor)
    fn_XDefineCursor(g_game_display, g_game_window, cursor);
  else if (fn_XUndefineCursor)
    fn_XUndefineCursor(g_game_display, g_game_window);
  if (g_overlay_cursor && fn_XFreeCursor)
    fn_XFreeCursor(g_game_display, g_overlay_cursor);
  g_overlay_cursor = cursor;
  if (fn_XFlush)
    fn_XFlush(g_game_display);
}

void idk_x11_cursor_dispatch(void) {
  if (!g_hook_installed || !g_captured || !g_game_display || !g_game_window)
    return;
  static uint8_t pixels[IDK_CURSOR_MAX_BYTES];
  idk_cursor_update_t cursor;
  uint32_t generation;
  if (!idk_input_cursor_snapshot(g_applied_cursor_generation, &cursor, pixels, sizeof(pixels), &generation))
    return;
  Cursor resource = 0;
  if (!cursor.visible || cursor.shape == IDK_CURSOR_CUSTOM) {
    resource = create_custom_cursor(&cursor, pixels);
  } else if (resolve_xcursor() == 0) {
    const char *name = cursor_name(cursor.shape);
    if (name)
      resource = fn_XcursorLibraryLoadCursor(g_game_display, name);
  }
  replace_cursor(resource);
  g_applied_cursor_generation = generation;
}

void idk_x11_cursor_shutdown(void) {
  if (g_game_display && g_game_window && fn_XUndefineCursor)
    fn_XUndefineCursor(g_game_display, g_game_window);
  if (g_overlay_cursor && g_game_display && fn_XFreeCursor)
    fn_XFreeCursor(g_game_display, g_overlay_cursor);
  g_overlay_cursor = 0;
  g_applied_cursor_generation = 0;
  if (g_xcursor_handle) {
    dlclose(g_xcursor_handle);
    g_xcursor_handle = NULL;
  }
}
int g_cursor_grabbed = 0;

/* Capture toggle */

void idk_x11_input_set_capture(int enable) {
  int new_state = enable ? 1 : 0;
  if (new_state == g_captured)
    return;
  g_captured = new_state;
  XLOG("set_capture(%s)", new_state ? "ON" : "OFF");
  if (new_state) {
    g_applied_cursor_generation = 0;
    idk_x11_cursor_dispatch();
  } else {
    replace_cursor(0);
    g_applied_cursor_generation = 0;
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
