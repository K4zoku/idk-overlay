/* X11 keyboard event mapping + hotkey detection.
 *
 * Hotkey behavior:
 *   - Capture hotkey: toggle capture. If ON → force show overlay.
 *   - Overlay hotkey: toggle overlay visibility independently.
 *   - If same key: press→capture ON+show, press→capture OFF (overlay stays).
 */
#include "hook/x11_internal.h"

extern _Atomic int g_webview_dead;

typedef struct {
    int type; unsigned long serial; Bool send_event; Display *display;
    Window window, root, subwindow; Time time; int x, y, x_root, y_root;
    unsigned int state, keycode, same_screen;
} XKeyEventLayout;

static void update_mods_from_state(unsigned int state) {
    uint32_t mods = 0;
    if (state & ShiftMask) mods |= IDK_MOD_SHIFT;
    if (state & ControlMask) mods |= IDK_MOD_CTRL;
    if (state & Mod1Mask) mods |= IDK_MOD_ALT;
    if (state & Mod4Mask) mods |= IDK_MOD_SUPER;
    g_mods = mods;
}

static uint32_t keycode_to_keysym(unsigned int keycode) {
    if (fn_XKeycodeToKeysym && g_game_display) {
        KeySym ks = fn_XKeycodeToKeysym(g_game_display, (KeyCode)keycode, 0);
        if (ks) return (uint32_t)ks;
    }
    return 0;
}

static uint32_t keycode_to_scancode(unsigned int keycode) {
    return (keycode >= 8) ? (keycode - 8) : keycode;
}

int x11_handle_key_event(XEventStorage *ev) {
    XKeyEventLayout *ke = (XKeyEventLayout *)ev;
    update_mods_from_state(ke->state);
    uint32_t scancode = keycode_to_scancode(ke->keycode);
    uint32_t keysym = keycode_to_keysym(ke->keycode);
    int pressed = (ke->type == KeyPress);

    int same_key = (g_hotkey_keysym == g_hotkey_overlay_keysym &&
                    g_hotkey_mods == g_hotkey_overlay_mods);

    /* Check capture hotkey */
    int cap_match = 0;
    if (g_hotkey_keysym || g_hotkey_scancode) {
        int m = 0;
        if (g_hotkey_keysym && keysym == g_hotkey_keysym) m = 1;
        if (g_hotkey_scancode && scancode == g_hotkey_scancode) m = 1;
        if (m && (!g_hotkey_mods || (g_mods & g_hotkey_mods) == g_hotkey_mods)) cap_match = 1;
    }

    /* Check overlay hotkey (only if different) */
    int ovl_match = 0;
    if (!same_key && (g_hotkey_overlay_keysym || g_hotkey_overlay_scancode)) {
        int m = 0;
        if (g_hotkey_overlay_keysym && keysym == g_hotkey_overlay_keysym) m = 1;
        if (g_hotkey_overlay_scancode && scancode == g_hotkey_overlay_scancode) m = 1;
        if (m && (!g_hotkey_overlay_mods || (g_mods & g_hotkey_overlay_mods) == g_hotkey_overlay_mods)) ovl_match = 1;
    }

    if (!g_webview_dead) {
    if (cap_match) {
        if (pressed && !g_hotkey_pressed) {
            g_hotkey_pressed = 1;
            if (same_key) {
                if (!g_captured) { idk_x11_input_set_capture(1); g_overlay_visible = 1; send_overlay_state(1); }
                else { idk_x11_input_set_capture(0); }
            } else {
                idk_x11_input_set_capture(!g_captured);
                if (g_captured) { g_overlay_visible = 1; send_overlay_state(1); }
            }
        } else if (!pressed) g_hotkey_pressed = 0;
        return 1;
    }

    if (ovl_match) {
        if (pressed && !g_hotkey_pressed) {
            g_hotkey_pressed = 1;
            g_overlay_visible = !g_overlay_visible;
            send_overlay_state(g_overlay_visible);
            XLOG("overlay %s", g_overlay_visible ? "SHOW" : "HIDE");
        } else if (!pressed) g_hotkey_pressed = 0;
        return 1;
    }
    }

    if (g_captured && pressed) g_hotkey_pressed = 0;
    if (!g_captured) return 0;

    idk_input_event_t ie = {0};
    ie.type = IDK_INPUT_KEY; ie.time = (uint32_t)ke->time;
    ie.u.key.keycode = scancode; ie.u.key.keysym = keysym; ie.u.key._p1 = 0;
    ie.flags = (pressed ? IDK_INPUT_FLAG_PRESS : 0) | IDK_INPUT_FLAG_CAPTURE;
    ie.mods = (uint16_t)g_mods;
    send_event_to_webview(&ie);
    return 1;
}
