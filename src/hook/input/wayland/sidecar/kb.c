#include "hook/wayland_internal.h"

#include "../internal.h"

extern _Atomic int g_webview_dead;

/* Sidecar keyboard listener */

static void sidecar_kb_keymap(void *d, struct wl_keyboard *kb, uint32_t fmt, int32_t fd, uint32_t sz) {
  (void)d;
  (void)kb;
  int dup_fd = -1;
  if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && g_xkb_handle && g_xkb_ctx) {
    dup_fd = dup(fd);
    if (dup_fd < 0)
      return;
  } else {
    return;
  }

  void *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, dup_fd, 0);
  if (map == MAP_FAILED) {
    close(dup_fd);
    return;
  }

  if (g_xkb_state && fn_xkb_state_unref) {
    fn_xkb_state_unref(g_xkb_state);
    g_xkb_state = NULL;
  }
  if (g_xkb_keymap && fn_xkb_keymap_unref) {
    fn_xkb_keymap_unref(g_xkb_keymap);
    g_xkb_keymap = NULL;
  }

  g_xkb_keymap = fn_xkb_keymap_new_from_string(g_xkb_ctx, (char *)map, IDK_XKB_KEYMAP_FORMAT_TEXT_V1,
                                               IDK_XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (g_xkb_keymap) {
    g_xkb_state = fn_xkb_state_new(g_xkb_keymap);
    if (g_xkb_state && fn_xkb_keymap_mod_get_index) {
      g_mod_idx_ctrl = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Control");
      g_mod_idx_shift = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Shift");
      g_mod_idx_alt = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Mod1");
      g_mod_idx_super = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Mod4");
    }
  }
  munmap(map, sz);
  close(dup_fd);
}

static void sidecar_kb_enter(void *d, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s,
                             struct wl_array *keys) {
  (void)d;
  (void)kb;
  (void)serial;
  (void)s;
  (void)keys;
}

static void sidecar_kb_leave(void *d, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s) {
  (void)d;
  (void)kb;
  (void)serial;
  (void)s;
}

static void sidecar_kb_key(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key,
                           uint32_t state) {
  (void)d;
  (void)kb;
  (void)serial;
  (void)time;

  if (key == 0 && state == 0)
    return;

  uint32_t keysym = 0;
  if (g_xkb_state && fn_xkb_state_key_get_one_sym)
    keysym = fn_xkb_state_key_get_one_sym(g_xkb_state, key + 8);

  if (g_xkb_state && fn_xkb_state_update_key)
    fn_xkb_state_update_key(g_xkb_state, key + 8, state ? IDK_XKB_KEY_DOWN : IDK_XKB_KEY_UP);

  int pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
  int same_key = (g_hotkey_keysym == g_hotkey_overlay_keysym && g_hotkey_mods == g_hotkey_overlay_mods);

  int cap_match = is_capture_hotkey(key, keysym);
  int ovl_match = !same_key && is_overlay_hotkey(key, keysym);

  if (!g_webview_dead) {
    if (cap_match) {
      if (pressed && !g_hotkey_pressed) {
        g_hotkey_pressed = 1;
        if (same_key) {
          if (!g_captured) {
            idk_wayland_input_set_capture(1);
            g_overlay_visible = 1;
            send_overlay_state(1);
          } else {
            idk_wayland_input_set_capture(0);
          }
        } else {
          idk_wayland_input_set_capture(!g_captured);
          if (g_captured) {
            g_overlay_visible = 1;
            send_overlay_state(1);
          }
        }
      } else if (!pressed) {
        g_hotkey_pressed = 0;
      }
    } else if (ovl_match) {
      if (pressed && !g_hotkey_pressed) {
        g_hotkey_pressed = 1;
        g_overlay_visible = !g_overlay_visible;
        send_overlay_state(g_overlay_visible);
        WLOG("sidecar overlay %s", g_overlay_visible ? "SHOW" : "HIDE");
      } else if (!pressed) {
        g_hotkey_pressed = 0;
      }
    }
  }
}

static void sidecar_kb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t dep, uint32_t lat,
                                 uint32_t lck, uint32_t grp) {
  (void)d;
  (void)kb;
  (void)serial;
  if (g_xkb_state && fn_xkb_state_update_mask) {
    fn_xkb_state_update_mask(g_xkb_state, dep, lat, lck, 0, 0, grp);
    update_mod_bitmask();
  }
}

static void sidecar_kb_repeat_info(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay) {
  (void)d;
  (void)kb;
  g_repeat_rate = rate;
  g_repeat_delay = delay;
  WLOG("sidecar repeat_info: rate=%d cps delay=%d ms", rate, delay);
  send_repeat_info();
}

IDK_INTERNAL const struct wl_keyboard_listener g_sidecar_kb_listener = {
    .keymap = sidecar_kb_keymap,
    .enter = sidecar_kb_enter,
    .leave = sidecar_kb_leave,
    .key = sidecar_kb_key,
    .modifiers = sidecar_kb_modifiers,
    .repeat_info = sidecar_kb_repeat_info,
};
