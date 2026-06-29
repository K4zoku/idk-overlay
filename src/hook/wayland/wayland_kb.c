#include "hook/wayland_internal.h"

extern _Atomic int g_webview_dead;

/* xkb globals */
void *g_xkb_handle = NULL;
struct xkb_context *g_xkb_ctx = NULL;
struct xkb_keymap *g_xkb_keymap = NULL;
struct xkb_state  *g_xkb_state  = NULL;

#define XKB_DEFINE(ret, name, params) name##_fn fn_##name = NULL;
XKB_FOREACH(XKB_DEFINE)
#undef XKB_DEFINE

uint32_t g_mod_idx_ctrl  = UINT32_MAX;
uint32_t g_mod_idx_shift = UINT32_MAX;
uint32_t g_mod_idx_alt   = UINT32_MAX;
uint32_t g_mod_idx_super = UINT32_MAX;

int32_t g_repeat_rate = 25;
int32_t g_repeat_delay = 500;

/* Forward declarations */
static void wkb_keymap(void *d, struct wl_keyboard *kb, uint32_t fmt, int32_t fd, uint32_t sz);
static void wkb_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
                      struct wl_surface *s, struct wl_array *keys);
static void wkb_leave(void *d, struct wl_keyboard *kb, uint32_t serial,
                      struct wl_surface *s);
static void wkb_key(void *d, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state);
static void wkb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial,
                          uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp);
static void wkb_repeat_info(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay);

/* Listener wrapper vtable (non-static - checked by proxy scan) */
const struct wl_keyboard_listener g_kb_wrapper = {
    .keymap       = wkb_keymap,
    .enter        = wkb_enter,
    .leave        = wkb_leave,
    .key          = wkb_key,
    .modifiers    = wkb_modifiers,
    .repeat_info  = wkb_repeat_info,
};

static uint32_t decode_keysym(uint32_t key);

/* Keyboard callbacks */

static void wkb_keymap(void *d, struct wl_keyboard *kb, uint32_t fmt, int32_t fd, uint32_t sz) {
    struct kb_state *st = (struct kb_state *)d;

    int dup_fd = -1;
    if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && g_xkb_handle && g_xkb_ctx) {
        dup_fd = dup(fd);
        if (dup_fd < 0)
            WLOG("keymap dup() failed: %s - xkb disabled", strerror(errno));
    }

    if (st->game && st->game->keymap)
        st->game->keymap(st->game_data, kb, fmt, fd, sz);

    if (dup_fd >= 0 && fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        void *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, dup_fd, 0);
        if (map == MAP_FAILED) {
            WLOG("keymap mmap failed: %s", strerror(errno));
        } else {
            if (g_xkb_state && fn_xkb_state_unref) {
                fn_xkb_state_unref(g_xkb_state);
                g_xkb_state = NULL;
            }
            if (g_xkb_keymap && fn_xkb_keymap_unref) {
                fn_xkb_keymap_unref(g_xkb_keymap);
                g_xkb_keymap = NULL;
            }

            g_xkb_keymap = fn_xkb_keymap_new_from_string(
                g_xkb_ctx, (char *)map,
                IDK_XKB_KEYMAP_FORMAT_TEXT_V1,
                IDK_XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (g_xkb_keymap) {
                g_xkb_state = fn_xkb_state_new(g_xkb_keymap);
                if (g_xkb_state) {
                    if (fn_xkb_keymap_mod_get_index) {
                        g_mod_idx_ctrl  = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Control");
                        g_mod_idx_shift = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Shift");
                        g_mod_idx_alt   = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Mod1");
                        g_mod_idx_super = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Mod4");
                    }
                    WLOG("xkb keymap loaded (mods: ctrl=%u shift=%u alt=%u super=%u)",
                         g_mod_idx_ctrl, g_mod_idx_shift,
                         g_mod_idx_alt, g_mod_idx_super);
                } else {
                    WLOG("xkb_state_new failed");
                }
            } else {
                WLOG("xkb_keymap_new_from_string failed");
            }
            munmap(map, sz);
        }
        close(dup_fd);
    }
}

static void wkb_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
                      struct wl_surface *s, struct wl_array *keys) {
    struct kb_state *st = (struct kb_state *)d;
    if (st->game && st->game->enter)
        st->game->enter(st->game_data, kb, serial, s, keys);
}

static void wkb_leave(void *d, struct wl_keyboard *kb, uint32_t serial,
                      struct wl_surface *s) {
    struct kb_state *st = (struct kb_state *)d;
    if (st->game && st->game->leave)
        st->game->leave(st->game_data, kb, serial, s);
}

static uint32_t decode_keysym(uint32_t key) {
    if (g_xkb_state && fn_xkb_state_key_get_one_sym)
        return fn_xkb_state_key_get_one_sym(g_xkb_state, key + 8);
    return 0;
}

int is_capture_hotkey(uint32_t key, uint32_t keysym) {
    int match = 0;
    if (g_hotkey_keysym && keysym == g_hotkey_keysym) match = 1;
    if (g_hotkey_scancode && key == g_hotkey_scancode) match = 1;
    if (!match) return 0;
    if (!g_hotkey_mods) return 1;
    return (g_mods & g_hotkey_mods) == g_hotkey_mods;
}

int is_overlay_hotkey(uint32_t key, uint32_t keysym) {
    int match = 0;
    if (g_hotkey_overlay_keysym && keysym == g_hotkey_overlay_keysym) match = 1;
    if (g_hotkey_overlay_scancode && key == g_hotkey_overlay_scancode) match = 1;
    if (!match) return 0;
    if (!g_hotkey_overlay_mods) return 1;
    return (g_mods & g_hotkey_overlay_mods) == g_hotkey_overlay_mods;
}

static void wkb_key(void *d, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state) {
    struct kb_state *st = (struct kb_state *)d;

    if (key == 0 && state == 0) return;

    uint32_t keysym = decode_keysym(key);

    if (g_xkb_state && fn_xkb_state_update_key)
        fn_xkb_state_update_key(g_xkb_state, key + 8,
                                state ? IDK_XKB_KEY_DOWN : IDK_XKB_KEY_UP);

    int pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    int same_key = (g_hotkey_keysym == g_hotkey_overlay_keysym &&
                    g_hotkey_mods == g_hotkey_overlay_mods);

    /* Check capture hotkey */
    int cap_match = is_capture_hotkey(key, keysym);

    /* Check overlay hotkey (only if different) */
    int ovl_match = !same_key && is_overlay_hotkey(key, keysym);

    if (!g_webview_dead) {
    if (cap_match) {
        if (pressed && !g_hotkey_pressed) {
            g_hotkey_pressed = 1;
            if (same_key) {
                if (!g_captured) { idk_wayland_input_set_capture(1); g_overlay_visible = 1; send_overlay_state(1); }
                else { idk_wayland_input_set_capture(0); }
            } else {
                idk_wayland_input_set_capture(!g_captured);
                if (g_captured) { g_overlay_visible = 1; send_overlay_state(1); }
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
        idk_input_event_t ev = { 0 };
        ev.type      = IDK_INPUT_KEY;
        ev.time      = time;
        ev.u.key.keycode = key;
        ev.u.key.keysym = keysym;
        ev.u.key._p1 = 0;
        ev.flags     = state ? IDK_INPUT_FLAG_PRESS : 0;
        ev.flags    |= IDK_INPUT_FLAG_CAPTURE;
        ev.mods      = (uint16_t)g_mods;
        WLOG("wkb_key: keycode=%u state=%u", key, state);
        send_event_to_webview(&ev);
        return;
    }
    if (st->game && st->game->key)
        st->game->key(st->game_data, kb, serial, time, key, state);
}

void update_mod_bitmask(void) {
    if (!g_xkb_state || !fn_xkb_state_mod_index_is_active) {
        g_mods = 0;
        return;
    }
    uint32_t m = 0;
    if (g_mod_idx_ctrl  != UINT32_MAX &&
        fn_xkb_state_mod_index_is_active(g_xkb_state, g_mod_idx_ctrl,  IDK_XKB_STATE_MODS_EFFECTIVE))
        m |= IDK_MOD_CTRL;
    if (g_mod_idx_shift != UINT32_MAX &&
        fn_xkb_state_mod_index_is_active(g_xkb_state, g_mod_idx_shift, IDK_XKB_STATE_MODS_EFFECTIVE))
        m |= IDK_MOD_SHIFT;
    if (g_mod_idx_alt   != UINT32_MAX &&
        fn_xkb_state_mod_index_is_active(g_xkb_state, g_mod_idx_alt,   IDK_XKB_STATE_MODS_EFFECTIVE))
        m |= IDK_MOD_ALT;
    if (g_mod_idx_super != UINT32_MAX &&
        fn_xkb_state_mod_index_is_active(g_xkb_state, g_mod_idx_super, IDK_XKB_STATE_MODS_EFFECTIVE))
        m |= IDK_MOD_SUPER;
    g_mods = m;
}

static void wkb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial,
                          uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) {
    struct kb_state *st = (struct kb_state *)d;

    if (g_xkb_state && fn_xkb_state_update_mask) {
        fn_xkb_state_update_mask(g_xkb_state, dep, lat, lck, 0, 0, grp);
        update_mod_bitmask();
    }

    if (g_captured) return;
    if (st->game && st->game->modifiers)
        st->game->modifiers(st->game_data, kb, serial, dep, lat, lck, grp);
}

static void wkb_repeat_info(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay) {
    struct kb_state *st = (struct kb_state *)d;

    g_repeat_rate = rate;
    g_repeat_delay = delay;
    WLOG("repeat_info: rate=%d cps delay=%d ms", rate, delay);
    send_repeat_info();

    if (st->game && st->game->repeat_info)
        st->game->repeat_info(st->game_data, kb, rate, delay);
}

void teardown_xkb(void) {
    if (g_xkb_state && fn_xkb_state_unref) {
        fn_xkb_state_unref(g_xkb_state);
        g_xkb_state = NULL;
    }
    if (g_xkb_keymap && fn_xkb_keymap_unref) {
        fn_xkb_keymap_unref(g_xkb_keymap);
        g_xkb_keymap = NULL;
    }
    if (g_xkb_ctx && fn_xkb_context_unref) {
        fn_xkb_context_unref(g_xkb_ctx);
        g_xkb_ctx = NULL;
    }
    if (g_xkb_handle) {
        dlclose(g_xkb_handle);
        g_xkb_handle = NULL;
    }
}
