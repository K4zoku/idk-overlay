#include "hook/wayland_internal.h"

#include "internal.h"

/* Resolved wayland function pointers */
#define WL_DEFINE(ret, name, params) name##_fn real_##name = NULL;
WL_FOREACH(WL_DEFINE)
#undef WL_DEFINE

/* Resolved xkbcommon function pointers */
#define XKB_DEFINE(ret, name, params) name##_fn fn_##name = NULL;
XKB_FOREACH(XKB_DEFINE)
#undef XKB_DEFINE

const struct wl_interface *g_wl_seat_interface = NULL;
const struct wl_interface *g_wl_keyboard_interface = NULL;
const struct wl_interface *g_wl_registry_interface = NULL;
const struct wl_interface *g_wl_pointer_interface = NULL;

void *g_wl_handle = NULL;

/* Resolve wayland symbols */
int resolve_wayland_symbols(void) {
  if (g_wl_handle)
    return 0;

  g_wl_handle = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (!g_wl_handle)
    g_wl_handle = dlopen("libwayland-client.so.0", RTLD_NOW);
  if (!g_wl_handle) {
    WLOG("libwayland-client.so.0 not loaded - input hook disabled");
    return -1;
  }

#define WL_RESOLVE(ret, name, params) real_##name = (name##_fn)dlsym(g_wl_handle, #name);
  WL_FOREACH(WL_RESOLVE)
#undef WL_RESOLVE

  if (!real_wl_proxy_add_listener || !real_wl_proxy_get_class) {
    WERR("failed to resolve core wayland symbols");
    dlclose(g_wl_handle);
    g_wl_handle = NULL;
    return -1;
  }

  g_wl_seat_interface = (const struct wl_interface *)dlsym(g_wl_handle, "wl_seat_interface");
  g_wl_keyboard_interface = (const struct wl_interface *)dlsym(g_wl_handle, "wl_keyboard_interface");
  g_wl_registry_interface = (const struct wl_interface *)dlsym(g_wl_handle, "wl_registry_interface");
  g_wl_pointer_interface = (const struct wl_interface *)dlsym(g_wl_handle, "wl_pointer_interface");

  WLOG("libwayland-client resolved: add_listener=%p get_class=%p sidecar=%s", (void *)real_wl_proxy_add_listener,
       (void *)real_wl_proxy_get_class, (real_wl_display_create_queue && g_wl_seat_interface) ? "OK" : "MISSING");
  return 0;
}

/* Resolve xkbcommon symbols */
int resolve_xkbcommon_symbols(void) {
  if (g_xkb_handle)
    return 0;

  g_xkb_handle = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (!g_xkb_handle)
    g_xkb_handle = dlopen("libxkbcommon.so.0", RTLD_NOW);
  if (!g_xkb_handle) {
    WLOG("libxkbcommon.so.0 not available - keysym translation disabled");
    return -1;
  }

#define XKB_RESOLVE(ret, name, params) fn_##name = (name##_fn)dlsym(g_xkb_handle, #name);
  XKB_FOREACH(XKB_RESOLVE)
#undef XKB_RESOLVE

  if (!fn_xkb_context_new || !fn_xkb_keymap_new_from_string || !fn_xkb_state_new || !fn_xkb_state_key_get_one_sym) {
    WERR("libxkbcommon missing required symbols");
    dlclose(g_xkb_handle);
    g_xkb_handle = NULL;
    return -1;
  }

  g_xkb_ctx = fn_xkb_context_new(IDK_XKB_CONTEXT_NO_FLAGS);
  if (!g_xkb_ctx) {
    WERR("xkb_context_new failed");
    return -1;
  }
  WLOG("libxkbcommon resolved, ctx=%p", (void *)g_xkb_ctx);
  return 0;
}
