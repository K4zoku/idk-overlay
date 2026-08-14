/*
 * idk_input.h - Input event wire type
 *
 * Input event (20 bytes, separate socket, no fd passing):
 *   +----------------------+
 *   | type   uint8         |  offset  0 - KEY/BUTTON/MOTION/AXIS/STATE/REPEAT
 *   | flags  uint8         |  offset  1 - bit0=press(1)/release(0)
 *   | mods   uint16        |  offset  2 - Ctrl=1,Shift=2,Alt=4,Super=8
 *   | time   uint32        |  offset  4 - wayland timestamp (ms)
 *   | payload union (12B)  |  offset  8 - key/btn/motion/axis/repeat/overlay
 *   +----------------------+  total 20 bytes
 *
 * Input events use a separate socket via idk_ipc_send/recv_input().
 *
 * The game (injected .so) hooks wl_proxy_add_listener to intercept
 * wl_pointer/wl_keyboard listeners. When "input capture" is toggled on
 * (default hotkey: F8), keyboard/mouse events are swallowed from the
 * game and forwarded to the webview via this protocol.
 *
 * Socket direction:
 *   - Game listens on $XDG_RUNTIME_DIR/idk-overlay-<pid>-input (server)
 *     (or /tmp/idk-overlay-<pid>-input if XDG_RUNTIME_DIR is unset)
 *   - Webview connects to it (client)
 *   - Game writes idk_input_event_t messages
 *   - Webview reads them in its event loop
 */
#ifndef IDK_INPUT_H
#define IDK_INPUT_H

#include <assert.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum idk_input_type {
  IDK_INPUT_KEY = 1,     /* keyboard press/release */
  IDK_INPUT_BUTTON = 2,  /* mouse button press/release */
  IDK_INPUT_MOTION = 3,  /* mouse motion (absolute, surface-local) */
  IDK_INPUT_AXIS = 4,    /* mouse scroll */
  IDK_INPUT_STATE = 5,   /* capture state changed (only flags bit0 matters) */
  IDK_INPUT_REPEAT = 6,  /* keyboard repeat info: rate (cps), delay (ms) */
  IDK_INPUT_OVERLAY = 7, /* overlay visibility changed */
};

/* Input event flags */
#define IDK_INPUT_FLAG_PRESS 0x01   /* bit0: 1=press, 0=release (KEY/BUTTON) */
#define IDK_INPUT_FLAG_CAPTURE 0x02 /* bit0 of state: capture ON when set   */

/* Modifier flags (XKB-style bitmask, can be combined) */
#define IDK_MOD_CTRL 0x01
#define IDK_MOD_SHIFT 0x02
#define IDK_MOD_ALT 0x04
#define IDK_MOD_SUPER 0x08

#pragma pack(push, 1)
typedef struct idk_input_event {
  uint8_t type;  /* offset  0 - IDK_INPUT_*                              */
  uint8_t flags; /* offset  1 - IDK_INPUT_FLAG_*                         */
  uint16_t mods; /* offset  2 - IDK_MOD_* bitmask                        */
  uint32_t time; /* offset  4 - wayland timestamp (ms)                   */
  union {        /* offset  8, size 12                                   */
    struct {
      uint32_t keycode;
      uint32_t keysym;
      uint32_t _p1;
    } key; /* KEY    */
    struct {
      int32_t x;
      int32_t y;
      uint32_t button;
    } btn; /* BUTTON: x,y first, then button */
    struct {
      int32_t x;
      int32_t y;
      uint32_t _p1;
    } motion; /* MOTION */
    struct {
      int32_t dx;
      int32_t dy;
      uint32_t _p1;
    } axis; /* AXIS   */
    struct {
      uint16_t rate;
      uint16_t delay;
      uint32_t _p1;
    } repeat; /* REPEAT */
    struct {
      uint8_t visible;
      uint8_t _pad[11];
    } overlay; /* OVERLAY */
  } u;
} idk_input_event_t; /* total 20 bytes                                       */
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_input_event_t) == 20, "idk_input_event_t must be 20 bytes");
#else
_Static_assert(sizeof(idk_input_event_t) == 20, "idk_input_event_t must be 20 bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* IDK_INPUT_H */
