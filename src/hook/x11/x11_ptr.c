/* x11_ptr.c — X11 pointer event mapping.
 *
 * Converts XButtonEvent (ButtonPress/ButtonRelease) and XMotionEvent
 * (MotionNotify) to idk_input_event_t and sends to webview when capture
 * is active.
 *
 * Button code translation:
 *   X11 Button1 (left)   → 0x110 (BTN_LEFT, wayland convention)
 *   X11 Button2 (middle) → 0x112 (BTN_MIDDLE)
 *   X11 Button3 (right)  → 0x111 (BTN_RIGHT)
 *   X11 Button4/5 (wheel up/down) → IDK_INPUT_AXIS with dy = ∓1
 *   X11 Button6/7 (wheel left/right) → IDK_INPUT_AXIS with dx = ∓1
 *
 * The webview side (input_receiver.cpp) expects wayland BTN_* constants
 * for button events — same convention as the Wayland input hook.
 */

#include "hook/x11_internal.h"

/* XButtonEvent layout (from Xlibint.h) */
typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    unsigned int button;
    unsigned int same_screen;
} XButtonEventLayout;

/* XMotionEvent layout (from Xlibint.h) */
typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    char is_hint;
    unsigned int same_screen;
} XMotionEventLayout;

/* Update g_mods from X11 state mask (same as x11_kb.c) */
static void update_mods_from_state(unsigned int state) {
    uint32_t mods = 0;
    if (state & ShiftMask)   mods |= IDK_MOD_SHIFT;
    if (state & ControlMask) mods |= IDK_MOD_CTRL;
    if (state & Mod1Mask)    mods |= IDK_MOD_ALT;
    if (state & Mod4Mask)    mods |= IDK_MOD_SUPER;
    g_mods = mods;
}

/* Translate X11 button number to wayland BTN_* constant.
 * Returns 0 for wheel buttons (4-7) which are handled as AXIS events. */
static uint32_t x11_button_to_wayland(unsigned int button) {
    switch (button) {
        case Button1: return 0x110;  /* BTN_LEFT */
        case Button2: return 0x112;  /* BTN_MIDDLE */
        case Button3: return 0x111;  /* BTN_RIGHT */
        default:      return 0;       /* wheel or unknown */
    }
}

/* Handle ButtonPress/ButtonRelease event.
 * Returns 1 if event should be swallowed (captured), 0 to forward. */
int x11_handle_button_event(XEventStorage *ev) {
    XButtonEventLayout *be = (XButtonEventLayout *)ev;
    update_mods_from_state(be->state);

    /* Track game window from button events too */
    if (!g_game_window && be->window) g_game_window = be->window;

    if (!g_captured) return 0;  /* forward to game */

    /* Wheel buttons (4/5/6/7) → AXIS events */
    if (be->button == Button4 || be->button == Button5 ||
        be->button == Button6 || be->button == Button7) {
        if (be->type != ButtonPress) return 1;  /* swallow release of wheel */
        idk_input_event_t ie = { 0 };
        ie.type   = IDK_INPUT_AXIS;
        ie.time   = (uint32_t)be->time;
        ie.mods   = (uint16_t)g_mods;
        ie.flags  = IDK_INPUT_FLAG_CAPTURE;
        /* Typical X11 wheel click = 1 unit. Wayland axis value is in
         * surface-coordinate units; webview scales by 12 (matching Qt's
         * default wheel step). Use ±12 for consistency. */
        const int32_t WHEEL_STEP = 12;
        switch (be->button) {
            case Button4: ie.u.axis.dy = -WHEEL_STEP; break;  /* up → negative */
            case Button5: ie.u.axis.dy =  WHEEL_STEP; break;  /* down → positive */
            case Button6: ie.u.axis.dx = -WHEEL_STEP; break;  /* left → negative */
            case Button7: ie.u.axis.dx =  WHEEL_STEP; break;  /* right → positive */
        }
        send_event_to_webview(&ie);
        return 1;  /* swallow */
    }

    /* Regular button → BUTTON event */
    uint32_t wl_button = x11_button_to_wayland(be->button);
    if (wl_button == 0) return 0;  /* unknown button, forward */

    idk_input_event_t ie = { 0 };
    ie.type        = IDK_INPUT_BUTTON;
    ie.time        = (uint32_t)be->time;
    ie.u.btn.x     = be->x;
    ie.u.btn.y     = be->y;
    ie.u.btn.button = wl_button;
    ie.flags       = (be->type == ButtonPress) ? IDK_INPUT_FLAG_PRESS : 0;
    ie.flags      |= IDK_INPUT_FLAG_CAPTURE;
    ie.mods        = (uint16_t)g_mods;
    send_event_to_webview(&ie);
    return 1;  /* swallow from game */
}

/* Handle MotionNotify event.
 * Returns 1 if event should be swallowed (captured), 0 to forward. */
int x11_handle_motion_event(XEventStorage *ev) {
    XMotionEventLayout *me = (XMotionEventLayout *)ev;
    update_mods_from_state(me->state);

    if (!g_game_window && me->window) g_game_window = me->window;

    if (!g_captured) return 0;  /* forward to game */

    idk_input_event_t ie = { 0 };
    ie.type        = IDK_INPUT_MOTION;
    ie.time        = (uint32_t)me->time;
    ie.u.motion.x  = me->x;
    ie.u.motion.y  = me->y;
    ie.u.motion._p1 = 0;
    ie.mods        = (uint16_t)g_mods;
    ie.flags       = IDK_INPUT_FLAG_CAPTURE;
    send_event_to_webview(&ie);
    return 1;  /* swallow from game */
}
