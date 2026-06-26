#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "public/idk_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IDK_TP_PATH_MAX 108

typedef enum {
    IDK_TP_CONSUMER,  /* creates endpoint, waits for producer */
    IDK_TP_PRODUCER,  /* connects to consumer's endpoint */
} idk_tp_role_t;

#define IDK_TP_SOCKET 0
#define IDK_TP_SHM    1

/* Opaque transport handle (64 bytes, stack-allocable). */
typedef struct idk_transport {
    idk_tp_role_t role;
    uint8_t       backend;      /* 0 = socket, 1 = SHM */
    bool          ready;

    /* ── backend-internal ────────────────────────────────────────────── */
    int           _server_fd;   /* socket: listen fd, shm: shm_fd   */
    int           _client_fd;   /* socket: connected client fd, shm: pidfd */
    uint8_t       _rsv[48];
} idk_transport_t;

_Static_assert(sizeof(idk_transport_t) == 64,
               "idk_transport_t must be 64 bytes");

/* ── Lifecycle ──────────────────────────────────────────────────────── */

int  idk_tp_init(idk_transport_t *tp, idk_tp_role_t role, const char *name);
void idk_tp_destroy(idk_transport_t *tp);

/* ── Consumer API ───────────────────────────────────────────────────── */

/* Non-blocking accept. Returns 1 on success, 0 if no pending, -1 on error. */
int  idk_tp_accept(idk_transport_t *tp);

/* Non-blocking poll. Returns 1 if data ready, 0 if not, -1 on error. */
int  idk_tp_poll(idk_transport_t *tp);

/* Receive frame header + fds. Returns 1 on success, 0 if no data,
 * -1 on error/disconnect. Caller owns received fds (must close). */
int  idk_tp_recv(idk_transport_t *tp, idk_frame_header_t *hdr,
                 int fds[4], int *nfd);

/* Send ACK to producer. */
void idk_tp_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack);

/* ── Producer API ───────────────────────────────────────────────────── */

/* Send frame header + fds. nfd=0 is invalid (use nfd=1 for fd 0 → SHM
 * path where fds[0] is a memfd). nfd=1..4 for DMABUF.
 * Returns 0 on success, -1 on error. */
int  idk_tp_send(idk_transport_t *tp, const idk_frame_header_t *hdr,
                 const int *fds, int nfd);

/* Block on ACK from consumer (up to timeout_ms). Returns 0 on success
 * with ack filled, -1 on timeout/error. */
int  idk_tp_wait_ack(idk_transport_t *tp, idk_ack_msg_t *ack, int timeout_ms);

#ifdef __cplusplus
}
#endif
