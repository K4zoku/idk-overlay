/* X11 pointer event mapping.
 *
 * Button code translation:
 *   X11 Button1 (left)   → 0x110 (BTN_LEFT, wayland convention)
 *   X11 Button2 (middle) → 0x112 (BTN_MIDDLE)
 *   X11 Button3 (right)  → 0x111 (BTN_RIGHT)
 *   X11 Button4/5 (wheel up/down) → IDK_INPUT_AXIS with dy = ∓1
 *   X11 Button6/7 (wheel left/right) → IDK_INPUT_AXIS with dx = ∓1
 *
 * The webview side (input_receiver.cpp) expects wayland BTN_* constants
 * for button events - same convention as the Wayland input hook.
 */
#include "hook/x11_internal.h"

/* Update g_mods from X11 state mask (same as kb.c) */
static void update_mods_from_state(unsigned int state) {
  uint32_t mods = 0;
  if (state & ShiftMask)
    mods |= IDK_MOD_SHIFT;
  if (state & ControlMask)
    mods |= IDK_MOD_CTRL;
  if (state & Mod1Mask)
    mods |= IDK_MOD_ALT;
  if (state & Mod4Mask)
    mods |= IDK_MOD_SUPER;
  g_mods = mods;
}

/* Translate X11 button number to wayland BTN_* constant.
 * Returns 0 for wheel buttons (4-7) which are handled as AXIS events. */
static uint32_t x11_button_to_wayland(unsigned int button) {
  switch (button) {
  case Button1:
    return 0x110;
  case Button2:
    return 0x112;
  case Button3:
    return 0x111;
  case Button8:
    return 0x113;
  case Button9:
    return 0x114;
  default:
    return 0;
  }
}

/* Handle ButtonPress/ButtonRelease event.
 * Returns 1 if event should be swallowed (captured), 0 to forward. */
int x11_handle_button_event(XEventStorage *ev) {
  XButtonEventLayout *be = (XButtonEventLayout *)ev;
  update_mods_from_state(be->state);

  if (!g_game_window && be->window)
    g_game_window = be->window;

  if (!g_captured)
    return 0;

  if (be->button == Button4 || be->button == Button5 || be->button == Button6 || be->button == Button7) {
    if (be->type != ButtonPress)
      return 1;
    idk_input_event_t ie = {0};
    ie.type = IDK_INPUT_AXIS;
    ie.time = (uint32_t)be->time;
    ie.mods = (uint16_t)g_mods;
    ie.flags = IDK_INPUT_FLAG_CAPTURE;
    const int32_t WHEEL_STEP = 12;
    switch (be->button) {
    case Button4:
      ie.u.axis.dy = -WHEEL_STEP;
      break;
    case Button5:
      ie.u.axis.dy = WHEEL_STEP;
      break;
    case Button6:
      ie.u.axis.dx = -WHEEL_STEP;
      break;
    case Button7:
      ie.u.axis.dx = WHEEL_STEP;
      break;
    }
    send_event_to_webview(&ie);
    return 1;
  }

  uint32_t wl_button = x11_button_to_wayland(be->button);
  if (wl_button == 0)
    return 0;

  idk_input_event_t ie = {0};
  ie.type = IDK_INPUT_BUTTON;
  ie.time = (uint32_t)be->time;
  ie.u.btn.x = be->x;
  ie.u.btn.y = be->y;
  ie.u.btn.button = wl_button;
  ie.flags = (be->type == ButtonPress) ? IDK_INPUT_FLAG_PRESS : 0;
  ie.flags |= IDK_INPUT_FLAG_CAPTURE;
  ie.mods = (uint16_t)g_mods;
  send_event_to_webview(&ie);
  return 1;
}

/* Handle MotionNotify event.
 * Returns 1 if event should be swallowed (captured), 0 to forward. */
int x11_handle_motion_event(XEventStorage *ev) {
  XMotionEventLayout *me = (XMotionEventLayout *)ev;
  update_mods_from_state(me->state);

  if (!g_game_window && me->window)
    g_game_window = me->window;

  if (!g_captured)
    return 0;

  idk_input_event_t ie = {0};
  ie.type = IDK_INPUT_MOTION;
  ie.time = (uint32_t)me->time;
  ie.u.motion.x = me->x;
  ie.u.motion.y = me->y;
  ie.u.motion._p1 = 0;
  ie.mods = (uint16_t)g_mods;
  ie.flags = IDK_INPUT_FLAG_CAPTURE;
  send_event_to_webview(&ie);
  return 1;
}
