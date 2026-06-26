/* x11_kb.c — X11 keyboard event mapping.
 *
 * Converts XKeyEvent (KeyPress/KeyRelease) to idk_input_event_t and
 * sends to webview when capture is active. Also handles hotkey detection.
 *
 * X11 keycode = evdev scancode + 8 (same convention as Wayland's
 * wl_keyboard key events). X11 keysyms (XStringToKeysym/XKeycodeToKeysym)
 * share the same numeric space as XKB keysyms — no translation needed.
 */

#include "hook/x11_internal.h"

/* XKeyEvent layout (from Xlibint.h) — first 5 fields are XAnyEventHeaders */
typedef struct {
    int type;              /* KeyPress or KeyRelease */
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;         /* window that received event */
    Window root;           /* root window event came from */
    Window subwindow;
    Time time;
    int x, y;              /* pointer coords relative to window */
    int x_root, y_root;    /* pointer coords relative to root */
    unsigned int state;    /* modifier mask (ShiftMask/ControlMask/etc) */
    unsigned int keycode;  /* hardware keycode (evdev scancode + 8) */
    unsigned int same_screen;
} XKeyEventLayout;

/* Update g_mods from X11 state mask */
static void update_mods_from_state(unsigned int state) {
    uint32_t mods = 0;
    if (state & ShiftMask)   mods |= IDK_MOD_SHIFT;
    if (state & ControlMask) mods |= IDK_MOD_CTRL;
    if (state & Mod1Mask)    mods |= IDK_MOD_ALT;
    if (state & Mod4Mask)    mods |= IDK_MOD_SUPER;
    g_mods = mods;
}

/* Convert X11 keycode to keysym using XKeycodeToKeysym (or fallback to 0) */
static uint32_t keycode_to_keysym(unsigned int keycode) {
    if (fn_XKeycodeToKeysym && g_game_display) {
        KeySym ks = fn_XKeycodeToKeysym(g_game_display, (KeyCode)keycode, 0);
        if (ks != 0) return (uint32_t)ks;
    }
    return 0;
}

/* Convert X11 keycode (evdev scancode + 8) to evdev scancode */
static uint32_t keycode_to_scancode(unsigned int keycode) {
    return (keycode >= 8) ? (keycode - 8) : keycode;
}

/* Handle KeyPress/KeyRelease event.
 * Returns 1 if event should be swallowed (captured or hotkey), 0 to forward. */
int x11_handle_key_event(XEventStorage *ev) {
    XKeyEventLayout *ke = (XKeyEventLayout *)ev;
    update_mods_from_state(ke->state);

    uint32_t scancode = keycode_to_scancode(ke->keycode);
    uint32_t keysym = keycode_to_keysym(ke->keycode);
    int pressed = (ke->type == KeyPress);

    XLOG("key_event: type=%s keycode=%u scancode=%u keysym=0x%x mods=0x%x captured=%d hotkey_pressed=%d",
         pressed ? "PRESS" : "RELEASE", ke->keycode, scancode, keysym, g_mods, g_captured, g_hotkey_pressed);

    /* Hotkey detection — check keysym OR scancode match (without mod check
     * for release). For press, require mods to match. For release, just
     * match the key itself so we can re-arm g_hotkey_pressed even if mods
     * were released first. */
    int key_matches = 0;
    if (g_hotkey_keysym && keysym == g_hotkey_keysym) key_matches = 1;
    if (g_hotkey_scancode && scancode == g_hotkey_scancode) key_matches = 1;

    if (key_matches) {
        XLOG("  hotkey match (key_matches=1, pressed=%d, g_hotkey_pressed=%d, mods_ok=%d)",
             pressed, g_hotkey_pressed,
             (!g_hotkey_mods) || ((g_mods & g_hotkey_mods) == g_hotkey_mods));
        if (pressed) {
            /* Toggle on every KeyPress of the hotkey (X autorepeat sends
             * multiple KeyPress/KeyRelease pairs while held — we want only
             * the first to toggle, but X may not send KeyRelease at all if
             * the game doesn't select for it. So: toggle only if we haven't
             * toggled on the previous KeyPress.
             *
             * If g_hotkey_pressed=0 (first press or reset): toggle + set flag.
             * If g_hotkey_pressed=1 (repeat or no release seen): just update
             * mods, don't toggle. The flag will be reset when we see a
             * KeyRelease OR when a non-hotkey key is pressed (below). */
            if (!g_hotkey_pressed) {
                int mods_ok = (!g_hotkey_mods) || ((g_mods & g_hotkey_mods) == g_hotkey_mods);
                if (mods_ok) {
                    g_hotkey_pressed = 1;
                    idk_x11_input_set_capture(!g_captured);
                }
            }
        } else {
            /* KeyRelease — re-arm */
            g_hotkey_pressed = 0;
        }
        return 1;  /* swallow hotkey always */
    }

    /* Non-hotkey key while captured — reset g_hotkey_pressed so the next
     * hotkey press can toggle. This handles the case where X doesn't send
     * KeyRelease for the hotkey (game doesn't select KeyReleaseMask).
     * As long as the user presses ANY other key between toggles, we re-arm. */
    if (g_captured && pressed) {
        g_hotkey_pressed = 0;
    }

    if (!g_captured) return 0;  /* forward to game */

    /* Send to webview */
    idk_input_event_t ie = { 0 };
    ie.type          = IDK_INPUT_KEY;
    ie.time          = (uint32_t)ke->time;
    ie.u.key.keycode = scancode;
    ie.u.key.keysym  = keysym;
    ie.u.key._p1     = 0;
    ie.flags         = pressed ? IDK_INPUT_FLAG_PRESS : 0;
    ie.flags        |= IDK_INPUT_FLAG_CAPTURE;
    ie.mods          = (uint16_t)g_mods;
    send_event_to_webview(&ie);
    return 1;  /* swallow from game */
}
