/*
 * idk_ipc.h — Unix domain socket IPC for passing dmabuf fds + metadata
 *
 * Wire format (P0.5 cleanup — 2026-06-26):
 *
 * Frame (28 bytes header + 1 fd via SCM_RIGHTS):
 *   +----------------------+
 *   | modifier uint64      |  offset  0 — DRM modifier (0=linear, SHM=0)
 *   | width     uint32     |  offset  8
 *   | height    uint32     |  offset 12
 *   | stride    uint32     |  offset 16 — bytes per row (DMABUF), 0=SHM
 *   | fourcc    uint32     |  offset 20 — DRM fourcc (DMABUF), 0=SHM
 *   | flags     uint8      |  offset 24 — bit0=visible, bit1=dmabuf
 *   | _pad     uint8[3]    |  offset 25 — reserved
 *   +----------------------+  total 28 bytes
 *
 * Input event (20 bytes, no fd passing):
 *   +----------------------+
 *   | type   uint8         |  offset  0 — KEY/BUTTON/MOTION/AXIS/STATE/REPEAT
 *   | flags  uint8         |  offset  1 — bit0=press(1)/release(0)
 *   | mods   uint16        |  offset  2 — Ctrl=1,Shift=2,Alt=4,Super=8
 *   | time   uint32        |  offset  4 — wayland timestamp (ms)
 *   | payload union (8B)   |  offset  8 — key/btn/motion/axis/repeat
 *   +----------------------+  total 16 bytes
 *
 * Removed (no backward compat needed — pre-release):
 *   magic, serial, capture, checksum, overlay_id, format, pixel_size, num_planes
 */
#ifndef IDK_IPC_H
#define IDK_IPC_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── IPC constants ─────────────────────────────────────────────────────── */

#define IDK_IPC_SOCKNAME_MAX 108    /* AF_UNIX path max */
#define IDK_IPC_SEND_BUF_SIZE 4096  /* CMSG space for 1 fd */

/* ── Frame header (28 bytes, sent with 1 fd via SCM_RIGHTS) ────────────── */

#define IDK_FRAME_FLAG_VISIBLE  0x01  /* bit0: overlay visible */
#define IDK_FRAME_FLAG_DMABUF   0x02  /* bit1: 1=dmabuf fd, 0=SHM fd */

#pragma pack(push, 1)
typedef struct idk_frame_header {
    uint64_t modifier;  /* offset  0 — DRM modifier (0=linear or SHM)        */
    uint32_t width;     /* offset  8 — frame width in pixels                  */
    uint32_t height;    /* offset 12 — frame height in pixels                 */
    uint32_t stride;    /* offset 16 — bytes per row (DMABUF), 0=SHM          */
    uint32_t fourcc;    /* offset 20 — DRM fourcc (DMABUF), 0=SHM             */
    uint8_t  flags;     /* offset 24 — IDK_FRAME_FLAG_*                       */
    uint8_t  _pad[3];   /* offset 25 — reserved, must be 0                    */
} idk_frame_header_t;   /* total 28 bytes                                     */
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_frame_header_t) == 28,
              "idk_frame_header_t must be 28 bytes");
#else
_Static_assert(sizeof(idk_frame_header_t) == 28,
               "idk_frame_header_t must be 28 bytes");
#endif

/* ── Input event (16 bytes, separate socket, no fd passing) ────────────── */
/*
 * The game (injected .so) hooks wl_proxy_add_listener to intercept
 * wl_pointer/wl_keyboard listeners. When "input capture" is toggled on
 * (default hotkey: F8), all keyboard/mouse events are swallowed from the
 * game and forwarded to the webview via this protocol so the overlay UI
 * can receive text input, clicks, etc.
 *
 * Socket direction:
 *   - Game listens on /tmp/idk-overlay-<pid>-input (server)
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
    uint8_t  type;   /* offset  0 — IDK_INPUT_*                              */
    uint8_t  flags;  /* offset  1 — IDK_INPUT_FLAG_*                         */
    uint16_t mods;   /* offset  2 — IDK_MOD_* bitmask                        */
    uint32_t time;   /* offset  4 — wayland timestamp (ms)                   */
    union {          /* offset  8, size 12                                   */
        struct { uint32_t keycode; uint32_t keysym; uint32_t _p1; } key;     /* KEY    */
        struct { int32_t  x;       int32_t  y; uint32_t button; } btn;      /* BUTTON: x,y first, then button */
        struct { int32_t  x;       int32_t  y; uint32_t _p1;  } motion;     /* MOTION */
        struct { int32_t  dx;      int32_t  dy; uint32_t _p1; } axis;       /* AXIS   */
        struct { uint16_t rate;    uint16_t delay; uint32_t _p1;            } repeat;/* REPEAT */
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

/* ── IPC socket management ─────────────────────────────────────────────── */

/**
 * Connect to the render process via Unix domain socket.
 * Creates the socket if it doesn't exist yet.
 */
int idk_ipc_connect(const char *sockpath, int *out_fd);

/**
 * Close the IPC socket connection.
 */
void idk_ipc_close(int fd);

/* ── Sending frames ────────────────────────────────────────────────────── */

/**
 * Send a frame header + fd over the IPC socket.
 * Uses SCM_RIGHTS to pass the fd atomically with the metadata.
 *
 * @param socket_fd   Connected socket fd.
 * @param hdr         Frame header (28 bytes, fully populated by caller).
 * @param fd          dmabuf/SHM fd to pass.
 * @return            0 on success, -1 on failure.
 */
int idk_ipc_send_frame(int socket_fd, const idk_frame_header_t *hdr, int fd);

/* ── Receiving frames ─────────────────────────────────────────────────── */

/**
 * Receive a frame header + fd from the IPC socket.
 * Blocks up to 2s waiting for data.
 *
 * @param socket_fd  Connected socket fd.
 * @param hdr        Output: frame header (28 bytes).
 * @param out_fd     Output: received fd (must be closed by caller).
 * @return           0 on success, -1 on EOF/error/timeout.
 */
int idk_ipc_recv_frame(int socket_fd, idk_frame_header_t *hdr, int *out_fd);

/* ── Input events (separate socket, no fd passing) ─────────────────────── */

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
