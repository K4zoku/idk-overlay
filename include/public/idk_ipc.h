/*
 * idk_ipc.h - Wire protocol types for idk-overlay
 *
 * Wire format (P0.5 cleanup - 2026-06-26):
 *
 * Frame (28 bytes header + fd via SCM_RIGHTS / pidfd_getfd):
 *   +----------------------+
 *   | modifier uint64      |  offset  0 - DRM modifier (0=linear, SHM=0)
 *   | width     uint32     |  offset  8
 *   | height    uint32     |  offset 12
 *   | stride    uint32     |  offset 16 - bytes per row (DMABUF), 0=SHM
 *   | fourcc    uint32     |  offset 20 - DRM fourcc (DMABUF), 0=SHM
 *   | flags     uint8      |  offset 24 - bit0=visible, bit1=dmabuf
 *   | nfd      uint8      |  offset 25 - fd count (1–4), ignored on recv
 *   | _pad     uint8[2]    |  offset 26 - reserved
 *   +----------------------+  total 28 bytes
 *
 * Input event (20 bytes, no fd passing):
 *   +----------------------+
 *   | type   uint8         |  offset  0 - KEY/BUTTON/MOTION/AXIS/STATE/REPEAT
 *   | flags  uint8         |  offset  1 - bit0=press(1)/release(0)
 *   | mods   uint16        |  offset  2 - Ctrl=1,Shift=2,Alt=4,Super=8
 *   | time   uint32        |  offset  4 - wayland timestamp (ms)
 *   | payload union (8B)   |  offset  8 - key/btn/motion/axis/repeat
 *   +----------------------+  total 16 bytes
 *
 * Frame transport is handled by idk_transport API (core/transport.h).
 * Input events use a separate socket via idk_ipc_send/recv_input().
 */
#ifndef IDK_IPC_H
#define IDK_IPC_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IPC constants */

#define IDK_IPC_SOCKNAME_MAX 108    /* AF_UNIX path max */

/* Frame header (28 bytes, sent with fds via transport) */

#define IDK_FRAME_FLAG_VISIBLE  0x01  /* bit0: overlay visible */
#define IDK_FRAME_FLAG_DMABUF   0x02  /* bit1: 1=dmabuf fd, 0=SHM fd */

#pragma pack(push, 1)
typedef struct idk_frame_header {
    uint64_t modifier;  /* offset  0 - DRM modifier (0=linear or SHM)        */
    uint32_t width;     /* offset  8 - frame width in pixels                  */
    uint32_t height;    /* offset 12 - frame height in pixels                 */
    uint32_t stride;    /* offset 16 - bytes per row (DMABUF), 0=SHM          */
    uint32_t fourcc;    /* offset 20 - DRM fourcc (DMABUF), 0=SHM             */
    uint8_t  flags;     /* offset 24 - IDK_FRAME_FLAG_*                       */
    uint8_t  nfd;       /* offset 25 - fd count (1–4 for send, 0 on recv)     */
    uint8_t  _pad[2];   /* offset 26 - reserved, must be 0                    */
} idk_frame_header_t;   /* total 28 bytes                                     */
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_frame_header_t) == 28,
              "idk_frame_header_t must be 28 bytes");
#else
_Static_assert(sizeof(idk_frame_header_t) == 28,
               "idk_frame_header_t must be 28 bytes");
#endif

/* ACK: 1B ack + 4B w + 4B h + 3B pad = 16B */

typedef struct idk_ack_msg {
    uint8_t  ack;     /* 0 = accepted, 1 = rejected (DMABUF not supported) */
    int32_t  w;       /* game width (0 = no resize) */
    int32_t  h;       /* game height (0 = no resize) */
    uint8_t  _pad[3]; /* reserved */
} idk_ack_msg_t;

/* REQUEST message (8 bytes, consumer→producer) */
/* Compositor sends this after presenting a frame to signal
 * "ready for next frame." Webview responds with a frame send. */

#define IDK_REQUEST_NEXT_FRAME 0
#define IDK_REQUEST_SHUTDOWN   1

#pragma pack(push, 1)
typedef struct idk_request_msg {
    uint8_t  type;       /* IDK_REQUEST_* */
    uint8_t  _pad[7];
} idk_request_msg_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_request_msg_t) == 8,
              "idk_request_msg_t must be 8 bytes");
#else
_Static_assert(sizeof(idk_request_msg_t) == 8,
               "idk_request_msg_t must be 8 bytes");
#endif

/* Input event (16 bytes, separate socket, no fd passing) */
/*
 * The game (injected .so) hooks wl_proxy_add_listener to intercept
 * wl_pointer/wl_keyboard listeners. When "input capture" is toggled on
 * (default hotkey: F8), all keyboard/mouse events are swallowed from the
 * game and forwarded to the webview via this protocol so the overlay UI
 * can receive text input, clicks, etc.
 *
 * Socket direction:
 *   - Game listens on $XDG_RUNTIME_DIR/idk-overlay-<pid>-input (server)
 *     (or /tmp/idk-overlay-<pid>-input if XDG_RUNTIME_DIR is unset)
 *   - Webview connects to it (client)
 *   - Game writes idk_input_event_t messages
 *   - Webview reads them in its event loop
 */

enum idk_input_type {
    IDK_INPUT_KEY     = 1,  /* keyboard press/release */
    IDK_INPUT_BUTTON  = 2,  /* mouse button press/release */
    IDK_INPUT_MOTION  = 3,  /* mouse motion (absolute, surface-local) */
    IDK_INPUT_AXIS    = 4,  /* mouse scroll */
    IDK_INPUT_STATE   = 5,  /* capture state changed (only flags bit0 matters) */
    IDK_INPUT_REPEAT  = 6,  /* keyboard repeat info: rate (cps), delay (ms) */
    IDK_INPUT_OVERLAY = 7,  /* overlay visibility changed */
};

/* Input event flags */
#define IDK_INPUT_FLAG_PRESS   0x01  /* bit0: 1=press, 0=release (KEY/BUTTON) */
#define IDK_INPUT_FLAG_CAPTURE 0x02  /* bit0 of state: capture ON when set   */

/* Modifier flags (XKB-style bitmask, can be combined) */
#define IDK_MOD_CTRL   0x01
#define IDK_MOD_SHIFT  0x02
#define IDK_MOD_ALT    0x04
#define IDK_MOD_SUPER  0x08

#pragma pack(push, 1)
typedef struct idk_input_event {
    uint8_t  type;   /* offset  0 - IDK_INPUT_*                              */
    uint8_t  flags;  /* offset  1 - IDK_INPUT_FLAG_*                         */
    uint16_t mods;   /* offset  2 - IDK_MOD_* bitmask                        */
    uint32_t time;   /* offset  4 - wayland timestamp (ms)                   */
    union {          /* offset  8, size 12                                   */
        struct { uint32_t keycode; uint32_t keysym; uint32_t _p1; } key;     /* KEY    */
        struct { int32_t  x;       int32_t  y; uint32_t button; } btn;      /* BUTTON: x,y first, then button */
        struct { int32_t  x;       int32_t  y; uint32_t _p1;  } motion;     /* MOTION */
        struct { int32_t  dx;      int32_t  dy; uint32_t _p1; } axis;       /* AXIS   */
        struct { uint16_t rate;    uint16_t delay; uint32_t _p1;            } repeat;/* REPEAT */
        struct { uint8_t visible;  uint8_t _pad[11];                       } overlay;/* OVERLAY */
    } u;
} idk_input_event_t;  /* total 20 bytes                                       */
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_input_event_t) == 20,
              "idk_input_event_t must be 20 bytes");
#else
_Static_assert(sizeof(idk_input_event_t) == 20,
               "idk_input_event_t must be 20 bytes");
#endif

/* Input events (separate socket, no fd passing) */

/**
 * Send an input event to the webview. Non-blocking: if the socket buffer
 * is full, the event is dropped.
 */
int idk_ipc_send_input(int socket_fd, const idk_input_event_t *ev);

/**
 * Receive an input event from the game. Blocking by default; pass
 * MSG_DONTWAIT for non-blocking poll.
 */
int idk_ipc_recv_input(int socket_fd, idk_input_event_t *ev, int flags);

#ifdef __cplusplus
}
#endif

#endif /* IDK_IPC_H */
