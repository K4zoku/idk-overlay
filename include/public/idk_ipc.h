/*
 * idk_ipc.h — Unix domain socket IPC for passing dmabuf fds + metadata
 *
 * Wire format per frame:
 *   [header: 32 bytes]          [scm_rights: 1 fd]
 *   +------------------+
 *   | width    uint32  |
 *   | height   uint32  |
 *   | stride   uint32  |
 *   | format   uint32  |
 *   | num_planes uint32|
 *   | pid      uint32  |
 *   | reserved uint32  |
 *   | checksum uint32  |
 *   +------------------+
 *
 * Input events use a separate socket (default path: /tmp/idk-overlay-<pid>-input)
 * to avoid multiplexing with the frame protocol. See idk_ipc_input_event_t.
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

/* ── Input event protocol (separate socket, no fd passing) ──────────────── */
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
 *   - Game writes idk_ipc_input_event_t messages
 *   - Webview reads them in its event loop
 *
 * Wire format (52 bytes, no SCM_RIGHTS):
 *   +----------------------+
 *   | magic    uint32      |  IDK_INPUT_MAGIC = 0x49444B49 ("IDKI")
 *   | type     uint32      |  IDK_INPUT_KEY / BUTTON / MOTION / AXIS / STATE
 *   | time     uint32      |  wayland timestamp (ms)
 *   | serial   uint32      |  wayland serial
 *   | keycode  uint32      |  evdev scancode (for KEY)
 *   | keysym   uint32      |  xkb keysym (for KEY, XKB_KEY_* from xkbcommon)
 *   | state    uint32      |  0=release, 1=press (for KEY/BUTTON)
 *   | button   uint32      |  BTN_* code (for BUTTON, linux/input-event-codes.h)
 *   | x        int32       |  surface-local x in pixels (for MOTION)
 *   | y        int32       |  surface-local y in pixels (for MOTION)
 *   | dx       int32       |  scroll delta (for AXIS, axis=0 is dx, axis=1 is dy)
 *   | dy       int32       |  scroll delta
 *   | mods     uint32      |  modifier bitmask (Ctrl=1, Shift=2, Alt=4, Super=8)
 *   | capture  uint32      |  0=passing through, 1=game input captured
 *   | reserved uint32      |  future use
 *   | checksum uint32      |  CRC32 of all preceding fields
 *   +----------------------+
 */

#define IDK_INPUT_MAGIC 0x49444B49u  /* "IDKI" */

enum idk_input_type {
    IDK_INPUT_KEY     = 1,  /* keyboard press/release */
    IDK_INPUT_BUTTON  = 2,  /* mouse button press/release */
    IDK_INPUT_MOTION  = 3,  /* mouse motion (absolute, surface-local) */
    IDK_INPUT_AXIS    = 4,  /* mouse scroll */
    IDK_INPUT_STATE   = 5,  /* capture state changed (only `capture` field matters) */
};

/* Modifier flags (XKB-style bitmask, can be combined) */
#define IDK_MOD_CTRL   0x01
#define IDK_MOD_SHIFT  0x02
#define IDK_MOD_ALT    0x04
#define IDK_MOD_SUPER  0x08

typedef struct idk_ipc_input_event {
    uint32_t magic;
    uint32_t type;
    uint32_t time;
    uint32_t serial;
    uint32_t keycode;
    uint32_t keysym;
    uint32_t state;
    uint32_t button;
    int32_t  x;
    int32_t  y;
    int32_t  dx;
    int32_t  dy;
    uint32_t mods;
    uint32_t capture;
    uint32_t reserved;
    uint32_t checksum;
} idk_ipc_input_event_t;  /* 16 × 4 = 64 bytes */

#ifdef __cplusplus
static_assert(sizeof(idk_ipc_input_event_t) == 64,
              "idk_ipc_input_event_t must be 64 bytes");
#else
_Static_assert(sizeof(idk_ipc_input_event_t) == 64,
               "idk_ipc_input_event_t must be 64 bytes");
#endif

/* ── IPC socket management ─────────────────────────────────────────────── */

/**
 * Connect to the render process via Unix domain socket.
 * Creates the socket if it doesn't exist yet.
 *
 * @param sockpath  Unix socket path.
 * @param out_fd    Output: the connected socket file descriptor.
 * @return          0 on success, -1 on failure.
 */
int idk_ipc_connect(const char *sockpath, int *out_fd);

/**
 * Close the IPC socket connection.
 *
 * @param fd  Socket fd from idk_ipc_connect().
 */
void idk_ipc_close(int fd);

/* ── Sending frames ────────────────────────────────────────────────────── */

/**
 * Send a frame info + dmabuf fd over the IPC socket.
 * Uses SCM_RIGHTS to pass the fd atomically with the metadata.
 *
 * @param socket_fd   Connected socket fd.
 * @param info        Frame metadata.
 * @param dmabuf_fd   DMABUF fd from the captured frame.
 * @return            0 on success, -1 on failure.
 */
int idk_ipc_send_frame(int socket_fd, const void *info, size_t info_len,
                       int dmabuf_fd);

/* ── Receiving frames (for idk-render process) ─────────────────────────── */

/**
 * Receive a frame info + fd from the IPC socket.
 * Blocks until a frame is available or connection is closed.
 *
 * @param socket_fd  Listening/connected socket fd.
 * @param info       Output: frame metadata struct.
 * @param out_fd     Output: received dmabuf fd (must be closed by caller).
 * @return           0 on success (frame received), -1 on EOF/error.
 */
int idk_ipc_recv_frame(int socket_fd, void *info, size_t info_len,
                       int *out_fd);

/* ── Input events (separate socket, no fd passing) ──────────────────────── */

/**
 * Send an input event to the webview. Non-blocking: if the socket buffer
 * is full, the event is dropped (better to drop than to stall the game's
 * input thread).
 *
 * @param socket_fd  Connected input socket fd.
 * @param ev         Input event to send (checksum is filled in by this call).
 * @return           0 on success, -1 on error or would-block.
 */
int idk_ipc_send_input(int socket_fd, const idk_ipc_input_event_t *ev);

/**
 * Receive an input event from the game. Blocking by default; pass
 * MSG_DONTWAIT (via flags param) for non-blocking poll.
 *
 * @param socket_fd  Connected input socket fd.
 * @param ev         Output: received event.
 * @param flags      recv() flags (0 = blocking, MSG_DONTWAIT = non-blocking).
 * @return           0 on success, -1 on EOF/error/again.
 */
int idk_ipc_recv_input(int socket_fd, idk_ipc_input_event_t *ev, int flags);

#ifdef __cplusplus
}
#endif

#endif /* IDK_IPC_H */
