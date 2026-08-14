#include "hook/syringe_hook.h"
#include "hook/wayland_internal.h"

#include "internal.h"

/* Capture state
 * Read from the compositor render path and written from the input hook
 * path. */
_Atomic int g_captured = 0;
_Atomic int g_hotkey_pressed = 0;
uint32_t g_hotkey_keysym = 0;
uint32_t g_hotkey_scancode = 0;
uint32_t g_hotkey_mods = 0;
uint32_t g_mods = 0;

static int g_hook_installed = 0;

/* Set capture */

void idk_wayland_input_set_capture(int enable) {
  int new_state = enable ? 1 : 0;
  if (new_state == g_captured)
    return;

  g_captured = new_state;

  if (idk_vk_layer_is_active()) {
    WLOG("set_capture(%s): skipping cursor ops (Vulkan layer mode)", new_state ? "ON" : "OFF");
    send_capture_state((uint32_t)new_state);
    if (new_state)
      send_repeat_info();
    return;
  }

  if (g_game_pointer_proxy) {
    sidecar_ensure_cursor_shape_device((struct wl_pointer *)g_game_pointer_proxy);

    uint32_t serial = g_last_enter_serial              ? g_last_enter_serial
                      : g_sidecar_pointer_enter_serial ? g_sidecar_pointer_enter_serial
                                                       : g_last_pointer_serial;

    if (new_state) {
      g_pre_capture_cursor_hidden = g_game_cursor_hidden;
      if (g_cursor_shape_device && serial) {
        WLOG("set_capture(ON): serial=%u shape=crosshair device=%p "
             "(enter=%u sidecar_enter=%u ptr=%u pre_hidden=%d)",
             serial, (void *)g_cursor_shape_device, g_last_enter_serial, g_sidecar_pointer_enter_serial,
             g_last_pointer_serial, g_pre_capture_cursor_hidden);
        my_wp_cursor_shape_device_set_shape(g_cursor_shape_device, serial, WP_CURSOR_SHAPE_DEFAULT);
      }
    } else {
      if (g_pointer_in_surface && g_game_pointer_proxy) {
        void **data_ptr = (void **)((char *)g_game_pointer_proxy + WL_PROXY_DATA_OFFSET);
        struct ptr_state *st = (struct ptr_state *)*data_ptr;
        if (st && st->game && st->game->motion) {
          WLOG("set_capture(OFF): re-sync motion to game (%d,%d)", g_cursor_x, g_cursor_y);
          st->game->motion(st->game_data, (struct wl_pointer *)g_game_pointer_proxy, 0, WL_INT_TO_FIXED(g_cursor_x),
                           WL_INT_TO_FIXED(g_cursor_y));
        }
      }
      if (g_cursor_shape_device && serial) {
        if (g_pre_capture_cursor_hidden) {
          WLOG("set_capture(OFF): serial=%u → hide cursor "
               "(pre-capture was hidden)",
               serial);
          my_wl_pointer_set_cursor(g_game_pointer_proxy, serial, NULL, 0, 0);
        } else {
          WLOG("set_capture(OFF): serial=%u shape=default device=%p", serial, (void *)g_cursor_shape_device);
          my_wp_cursor_shape_device_set_shape(g_cursor_shape_device, serial, WP_CURSOR_SHAPE_DEFAULT);
        }
      }
    }
  } else if (!new_state) {
    WLOG("set_capture(OFF): no game pointer proxy - cursor unchanged");
  }

  WLOG("input capture %s", new_state ? "ENABLED" : "DISABLED");
  send_capture_state((uint32_t)new_state);

  if (new_state)
    send_repeat_info();
}

int idk_wayland_input_is_captured(void) { return g_captured; }

/* Init */

int idk_wayland_input_init(void) {
  if (g_hook_installed)
    return 0;

  if (resolve_wayland_symbols() != 0)
    return -1;

  resolve_xkbcommon_symbols();
  configure_hotkey();

  if (init_input_socket() != 0)
    WLOG("input socket init failed - events will be dropped (no webview)");

  int n = syringe_hook_install("wl_proxy_add_listener", (void *)hook_wl_proxy_add_listener,
                               (void **)&orig_wl_proxy_add_listener);
  if (n <= 0) {
    WERR("syringe_hook_install(wl_proxy_add_listener) failed: n=%d", n);
    return -1;
  }

  int n2 = syringe_hook_install("wl_proxy_add_dispatcher", (void *)hook_wl_proxy_add_dispatcher,
                                (void **)&orig_wl_proxy_add_dispatcher);
  if (n2 <= 0)
    WLOG("wl_proxy_add_dispatcher hook not installed (n=%d)", n2);

  int n3 =
      syringe_hook_install("wl_display_connect", (void *)hook_wl_display_connect, (void **)&orig_wl_display_connect);
  if (n3 <= 0)
    WLOG("wl_display_connect hook not installed (n=%d)", n3);

  int n4 = syringe_hook_install("wl_display_connect_to_fd", (void *)hook_wl_display_connect_to_fd,
                                (void **)&orig_wl_display_connect_to_fd);
  if (n4 <= 0)
    WLOG("wl_display_connect_to_fd hook not installed (n=%d)", n4);

  int n5 = syringe_hook_install("wl_display_dispatch_queue_pending", (void *)hook_wl_display_dispatch_queue_pending,
                                (void **)&orig_wl_display_dispatch_queue_pending);
  if (n5 <= 0)
    WLOG("wl_display_dispatch_queue_pending hook not installed (n=%d)", n5);

  int n6 = syringe_hook_install("wl_proxy_marshal_array_flags", (void *)hook_wl_proxy_marshal_array_flags,
                                (void **)&orig_wl_proxy_marshal_array_flags);
  if (n6 <= 0)
    WLOG("wl_proxy_marshal_array_flags hook not installed (n=%d)", n6);

  int n7 = syringe_hook_install("wl_proxy_destroy", (void *)hook_wl_proxy_destroy, (void **)&orig_wl_proxy_destroy);
  if (n7 <= 0)
    WLOG("wl_proxy_destroy hook not installed (n=%d)", n7);

  g_hook_installed = 1;
  WLOG("hooks installed: add_listener=%d add_dispatcher=%d "
       "display_connect=%d display_connect_to_fd=%d dispatch_queue_pending=%d "
       "marshal_array=%d proxy_destroy=%d",
       n, n2, n3, n4, n5, n6, n7);
  return 0;
}

/* Shutdown */

void idk_wayland_input_shutdown(void) {
  if (!g_hook_installed)
    return;

  g_hook_installed = 0;
  g_sidecar_initialized = 0;
  g_sidecar_display = NULL;
  g_sidecar_keyboard = NULL;
  g_sidecar_seat = NULL;
  g_sidecar_cursor_shape_manager = NULL;
  g_sidecar_queue = NULL;

  g_cursor_shape_device = NULL;
  g_shape_device_pointer_proxy = NULL;

  orig_wl_proxy_destroy = NULL;

  teardown_input_socket();
  teardown_xkb();

  if (g_wl_handle) {
    dlclose(g_wl_handle);
    g_wl_handle = NULL;
  }
}
