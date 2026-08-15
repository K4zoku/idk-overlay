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
 * The channel is bidirectional:
 *   - The game writes idk_input_event_t messages to the webview.
 *   - The webview writes idk_cursor_update_t plus optional BGRA pixels
 *     back to the game.
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

#define IDK_CURSOR_MAGIC 0x52554B49u
#define IDK_CURSOR_VERSION 1u
#define IDK_CURSOR_MAX_DIM 256u
#define IDK_CURSOR_MAX_BYTES (IDK_CURSOR_MAX_DIM * IDK_CURSOR_MAX_DIM * 4u)
#define IDK_CURSOR_SCALE_BASE 100u

enum idk_cursor_shape {
  IDK_CURSOR_DEFAULT = 1,
  IDK_CURSOR_CONTEXT_MENU = 2,
  IDK_CURSOR_HELP = 3,
  IDK_CURSOR_POINTER = 4,
  IDK_CURSOR_PROGRESS = 5,
  IDK_CURSOR_WAIT = 6,
  IDK_CURSOR_CELL = 7,
  IDK_CURSOR_CROSSHAIR = 8,
  IDK_CURSOR_TEXT = 9,
  IDK_CURSOR_VERTICAL_TEXT = 10,
  IDK_CURSOR_ALIAS = 11,
  IDK_CURSOR_COPY = 12,
  IDK_CURSOR_MOVE = 13,
  IDK_CURSOR_NO_DROP = 14,
  IDK_CURSOR_NOT_ALLOWED = 15,
  IDK_CURSOR_GRAB = 16,
  IDK_CURSOR_GRABBING = 17,
  IDK_CURSOR_E_RESIZE = 18,
  IDK_CURSOR_N_RESIZE = 19,
  IDK_CURSOR_NE_RESIZE = 20,
  IDK_CURSOR_NW_RESIZE = 21,
  IDK_CURSOR_S_RESIZE = 22,
  IDK_CURSOR_SE_RESIZE = 23,
  IDK_CURSOR_SW_RESIZE = 24,
  IDK_CURSOR_W_RESIZE = 25,
  IDK_CURSOR_EW_RESIZE = 26,
  IDK_CURSOR_NS_RESIZE = 27,
  IDK_CURSOR_NESW_RESIZE = 28,
  IDK_CURSOR_NWSE_RESIZE = 29,
  IDK_CURSOR_COL_RESIZE = 30,
  IDK_CURSOR_ROW_RESIZE = 31,
  IDK_CURSOR_ALL_SCROLL = 32,
  IDK_CURSOR_ZOOM_IN = 33,
  IDK_CURSOR_ZOOM_OUT = 34,
  IDK_CURSOR_DND_ASK = 35,
  IDK_CURSOR_ALL_RESIZE = 36,
  IDK_CURSOR_CUSTOM = 255,
};

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

#pragma pack(push, 1)
typedef struct idk_cursor_update {
  uint32_t magic;
  uint8_t version;
  uint8_t visible;
  uint8_t shape;
  uint8_t _pad0;
  uint16_t width;
  uint16_t height;
  int16_t hotspot_x;
  int16_t hotspot_y;
  uint16_t scale;
  uint16_t _pad1;
  uint32_t data_size;
} idk_cursor_update_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_input_event_t) == 20, "idk_input_event_t must be 20 bytes");
static_assert(sizeof(idk_cursor_update_t) == 24, "idk_cursor_update_t must be 24 bytes");
#else
_Static_assert(sizeof(idk_input_event_t) == 20, "idk_input_event_t must be 20 bytes");
_Static_assert(sizeof(idk_cursor_update_t) == 24, "idk_cursor_update_t must be 24 bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* IDK_INPUT_H */
