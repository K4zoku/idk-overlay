/* X11 input backend - symbol resolution + lifecycle.
 *
 * Wayland and X11 are mutually exclusive in practice (games use one or
 * the other), but both hook sets can coexist - whichever library is
 * loaded installs its hook. The shared state ensures only one capture
 * toggle exists at runtime.
 */
#include "hook/syringe_hook.h"
#include "hook/x11_internal.h"

/* X11-specific globals (NOT shared with Wayland) */
void *g_x11_handle = NULL;
int g_hook_installed = 0;

#define X11_DEFINE_FN(ret, name, params) name##_fn fn_##name = NULL;
X11_CURSOR_FOREACH(X11_DEFINE_FN)
#undef X11_DEFINE_FN

/* Symbol resolution */

static int resolve_x11_symbols(void) {
  if (g_x11_handle)
    return 0;

  void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
  if (!h)
    h = dlopen("libX11.so.6", RTLD_NOW);
  if (!h)
    h = dlopen("libX11.so", RTLD_NOW);
  if (!h) {
    XERR("dlopen libX11 failed: %s", dlerror());
    return -1;
  }
  g_x11_handle = h;

#define X11_RESOLVE(ret, name, params) fn_##name = (name##_fn)dlsym(h, #name);
  X11_CURSOR_FOREACH(X11_RESOLVE)
#undef X11_RESOLVE

  if (!fn_XStringToKeysym) {
    XERR("XStringToKeysym not resolved\n");
    return -1;
  }

  XLOG("libX11 resolved: XKeycodeToKeysym=%p XStringToKeysym=%p XDefineCursor=%p XGrabPointer=%p",
       (void *)fn_XKeycodeToKeysym, (void *)fn_XStringToKeysym, (void *)fn_XDefineCursor, (void *)fn_XGrabPointer);
  return 0;
}

int idk_x11_input_init(void) {
  if (g_hook_installed)
    return 0;

  if (resolve_x11_symbols() != 0)
    return -1;

  configure_hotkey();

  if (init_input_socket() != 0)
    XERR("input socket init failed - events will be dropped");

#define INSTALL(name) syringe_hook_install(#name, (void *)hook_##name, (void **)&orig_##name)

  int n = 0;
  XLOG("install XNextEvent");
  n += INSTALL(XNextEvent);
  XLOG("install XPeekEvent");
  n += INSTALL(XPeekEvent);
  XLOG("install XCheckWindowEvent");
  n += INSTALL(XCheckWindowEvent);
  XLOG("install XMaskEvent");
  n += INSTALL(XMaskEvent);
  XLOG("install XCheckMaskEvent");
  n += INSTALL(XCheckMaskEvent);
  XLOG("install XCheckTypedEvent");
  n += INSTALL(XCheckTypedEvent);
  XLOG("install XCheckTypedWindowEvent");
  n += INSTALL(XCheckTypedWindowEvent);
  XLOG("install XWindowEvent");
  n += INSTALL(XWindowEvent);
  XLOG("install XIfEvent");
  n += INSTALL(XIfEvent);
  XLOG("install XCheckIfEvent");
  n += INSTALL(XCheckIfEvent);
  XLOG("install XSelectInput");
  n += INSTALL(XSelectInput);

#undef INSTALL

  if (n == 0) {
    XERR("no X11 hooks installed\n");
    return -1;
  }

  g_hook_installed = 1;
  XLOG("hooks installed: %d XNextEvent-family functions", n);
  return 0;
}

void idk_x11_input_shutdown(void) {
  if (!g_hook_installed)
    return;
  g_hook_installed = 0;

  if (g_cursor_grabbed && g_game_display && fn_XUngrabPointer) {
    fn_XUngrabPointer(g_game_display, 0);
    g_cursor_grabbed = 0;
  }
  idk_x11_cursor_shutdown();

  if (g_input_listen_fd >= 0) {
    teardown_input_socket();
  }

  if (g_x11_handle) {
    dlclose(g_x11_handle);
    g_x11_handle = NULL;
  }
}
