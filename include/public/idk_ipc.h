/*
 * idk_ipc.h - Wire protocol types for idk-overlay (umbrella)
 *
 * Wire format (P0.5 cleanup - 2026-06-26):
 *
 * Frame (28 bytes header + fd via SCM_RIGHTS / pidfd_getfd)
 * Input event (20 bytes, no fd passing)
 *
 * Frame transport is handled by idk_transport API (core/transport.h).
 * Input events use a separate socket via idk_ipc_send/recv_input().
 *
 * Split into protocol-specific headers:
 *   idk_cp.h     - broker control-plane handshake (204B)
 *   idk_frames.h - frame header (28B) + ACK (16B) + request (8B)
 *   idk_input.h  - input event (20B) + union
 */
#ifndef IDK_IPC_H
#define IDK_IPC_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "idk_cp.h"
#include "idk_frames.h"
#include "idk_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IPC constants */

#define IDK_IPC_SOCKNAME_MAX 108 /* AF_UNIX path max */

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
