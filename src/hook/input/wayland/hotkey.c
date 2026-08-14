#include "hook/wayland_internal.h"

#include "internal.h"

extern _Atomic int g_webview_dead;

/* Hotkey table */
struct hotkey_name_to_scancode {
  const char *name;
  uint32_t scancode;
  uint32_t keysym;
};

static const struct hotkey_name_to_scancode HOTKEY_TABLE[] = {
    {"Tab", IDK_KEY_TAB, IDK_XKB_KEY_Tab},       {"F1", IDK_KEY_F1, IDK_XKB_KEY_F1},
    {"F2", IDK_KEY_F2, IDK_XKB_KEY_F2},          {"F3", IDK_KEY_F3, IDK_XKB_KEY_F3},
    {"F4", IDK_KEY_F4, IDK_XKB_KEY_F4},          {"F5", IDK_KEY_F5, IDK_XKB_KEY_F5},
    {"F6", IDK_KEY_F6, IDK_XKB_KEY_F6},          {"F7", IDK_KEY_F7, IDK_XKB_KEY_F7},
    {"F8", IDK_KEY_F8, IDK_XKB_KEY_F8},          {"F9", IDK_KEY_F9, IDK_XKB_KEY_F9},
    {"F10", IDK_KEY_F10, IDK_XKB_KEY_F10},       {"F11", IDK_KEY_F11, IDK_XKB_KEY_F11},
    {"F12", IDK_KEY_F12, IDK_XKB_KEY_F12},       {"Scroll_Lock", IDK_KEY_SCROLLLOCK, IDK_XKB_KEY_Scroll_Lock},
    {"Pause", IDK_KEY_PAUSE, IDK_XKB_KEY_Pause},
};

void configure_hotkey(void) {
  const char *env = getenv("IDK_HOTKEY_CAPTURE");
  if (!env || !env[0])
    env = "Shift+Tab";

  uint32_t mods = 0;
  const char *keyname = env;

  const char *plus = strchr(env, '+');
  if (plus && plus > env) {
    size_t n = (size_t)(plus - env);
    char mod[32];
    if (n < sizeof(mod)) {
      memcpy(mod, env, n);
      mod[n] = '\0';
      if (strcasecmp(mod, "Shift") == 0)
        mods = IDK_MOD_SHIFT;
      else if (strcasecmp(mod, "Ctrl") == 0)
        mods = IDK_MOD_CTRL;
      else if (strcasecmp(mod, "Alt") == 0)
        mods = IDK_MOD_ALT;
      else if (strcasecmp(mod, "Super") == 0)
        mods = IDK_MOD_SUPER;
      keyname = plus + 1;
    }
  }
  g_hotkey_mods = mods;

  if (fn_xkb_keysym_from_name) {
    uint32_t ks = fn_xkb_keysym_from_name(keyname, IDK_XKB_KEYSYM_NO_FLAGS);
    if (ks != 0) {
      g_hotkey_keysym = ks;
      for (size_t i = 0; i < sizeof(HOTKEY_TABLE) / sizeof(HOTKEY_TABLE[0]); i++) {
        if (strcmp(keyname, HOTKEY_TABLE[i].name) == 0) {
          g_hotkey_scancode = HOTKEY_TABLE[i].scancode;
          break;
        }
      }
      WLOG("hotkey: %s (mods=0x%x keysym=0x%x scancode=%u)", env, g_hotkey_mods, g_hotkey_keysym, g_hotkey_scancode);
      return;
    }
  }

  for (size_t i = 0; i < sizeof(HOTKEY_TABLE) / sizeof(HOTKEY_TABLE[0]); i++) {
    if (strcmp(keyname, HOTKEY_TABLE[i].name) == 0) {
      g_hotkey_keysym = HOTKEY_TABLE[i].keysym;
      g_hotkey_scancode = HOTKEY_TABLE[i].scancode;
      WLOG("hotkey (from table): %s (mods=0x%x keysym=0x%x scancode=%u)", env, g_hotkey_mods, g_hotkey_keysym,
           g_hotkey_scancode);
      return;
    }
  }

  g_hotkey_keysym = IDK_XKB_KEY_Tab;
  g_hotkey_scancode = IDK_KEY_TAB;
  g_hotkey_mods = IDK_MOD_SHIFT;
  WLOG("unknown hotkey '%s', falling back to Shift+Tab", env);
}

int is_capture_hotkey(uint32_t key, uint32_t keysym) {
  int match = 0;
  if (g_hotkey_keysym && keysym == g_hotkey_keysym)
    match = 1;
  if (g_hotkey_scancode && key == g_hotkey_scancode)
    match = 1;
  if (!match)
    return 0;
  if (!g_hotkey_mods)
    return 1;
  return (g_mods & g_hotkey_mods) == g_hotkey_mods;
}

int is_overlay_hotkey(uint32_t key, uint32_t keysym) {
  int match = 0;
  if (g_hotkey_overlay_keysym && keysym == g_hotkey_overlay_keysym)
    match = 1;
  if (g_hotkey_overlay_scancode && key == g_hotkey_overlay_scancode)
    match = 1;
  if (!match)
    return 0;
  if (!g_hotkey_overlay_mods)
    return 1;
  return (g_mods & g_hotkey_overlay_mods) == g_hotkey_overlay_mods;
}

void wkb_key(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  struct kb_state *st = (struct kb_state *)d;

  if (key == 0 && state == 0)
    return;

  uint32_t keysym = decode_keysym(key);

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
      return;
    }

    if (ovl_match) {
      if (pressed && !g_hotkey_pressed) {
        g_hotkey_pressed = 1;
        g_overlay_visible = !g_overlay_visible;
        send_overlay_state(g_overlay_visible);
        WLOG("overlay %s", g_overlay_visible ? "SHOW" : "HIDE");
      } else if (!pressed) {
        g_hotkey_pressed = 0;
      }
      return;
    }
  }

  if (g_captured) {
    idk_input_event_t ev = {0};
    ev.type = IDK_INPUT_KEY;
    ev.time = time;
    ev.u.key.keycode = key;
    ev.u.key.keysym = keysym;
    ev.u.key._p1 = 0;
    ev.flags = state ? IDK_INPUT_FLAG_PRESS : 0;
    ev.flags |= IDK_INPUT_FLAG_CAPTURE;
    ev.mods = (uint16_t)g_mods;
    WLOG("wkb_key: keycode=%u state=%u", key, state);
    send_event_to_webview(&ev);
    return;
  }
  if (st->game && st->game->key)
    st->game->key(st->game_data, kb, serial, time, key, state);
}
