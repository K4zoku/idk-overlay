/* transport.c — Transport abstraction dispatch layer.
 *
 * Routes idk_transport API calls to the appropriate backend.
 * Backend selection:
 *   IDK_TP_BACKEND=shm  → SHM+futex (requires kernel 5.6+)
 *   (default)            → Unix domain socket
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/transport.h"

/* Backend declarations */
extern int  tp_socket_init(idk_transport_t *tp, const char *name);
extern void tp_socket_destroy(idk_transport_t *tp);
extern int  tp_socket_accept(idk_transport_t *tp);
extern int  tp_socket_poll(idk_transport_t *tp);
extern int  tp_socket_recv(idk_transport_t *tp, idk_frame_header_t *hdr,
                           int fds[4], int *nfd);
extern void tp_socket_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack);
extern int  tp_socket_send(idk_transport_t *tp, const idk_frame_header_t *hdr,
                           const int *fds, int nfd);
extern int  tp_socket_wait_ack(idk_transport_t *tp, idk_ack_msg_t *ack,
                                int timeout_ms);
extern int  tp_socket_send_request(idk_transport_t *tp, const idk_request_msg_t *req);
extern int  tp_socket_recv_request(idk_transport_t *tp, idk_request_msg_t *req,
                                    int timeout_ms);

extern int  tp_shm_init(idk_transport_t *tp, const char *name);
extern void tp_shm_destroy(idk_transport_t *tp);
extern int  tp_shm_accept(idk_transport_t *tp);
extern int  tp_shm_poll(idk_transport_t *tp);
extern int  tp_shm_recv(idk_transport_t *tp, idk_frame_header_t *hdr,
                        int fds[4], int *nfd);
extern void tp_shm_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack);
extern int  tp_shm_send(idk_transport_t *tp, const idk_frame_header_t *hdr,
                        const int *fds, int nfd);
extern int  tp_shm_wait_ack(idk_transport_t *tp, idk_ack_msg_t *ack,
                             int timeout_ms);
extern int  tp_shm_send_request(idk_transport_t *tp, const idk_request_msg_t *req);
extern int  tp_shm_recv_request(idk_transport_t *tp, idk_request_msg_t *req,
                                 int timeout_ms);

/* ── Helpers ──────────────────────────────────────────────────────────── */

static int resolve_backend(void) {
    const char *env = getenv("IDK_TP_BACKEND");
    if (env && strcasecmp(env, "shm") == 0)
        return IDK_TP_SHM;
    return IDK_TP_SOCKET;
}

/* ── Dispatch helpers ─────────────────────────────────────────────────── */

static int  (* const init_fn[2])(idk_transport_t *, const char *)   = { tp_socket_init, tp_shm_init };
static void (* const destroy_fn[2])(idk_transport_t *)               = { tp_socket_destroy, tp_shm_destroy };
static int  (* const accept_fn[2])(idk_transport_t *)                = { tp_socket_accept, tp_shm_accept };
static int  (* const poll_fn[2])(idk_transport_t *)                  = { tp_socket_poll, tp_shm_poll };
static int  (* const recv_fn[2])(idk_transport_t *, idk_frame_header_t *, int[4], int *) = { tp_socket_recv, tp_shm_recv };
static void (* const send_ack_fn[2])(idk_transport_t *, const idk_ack_msg_t *) = { tp_socket_send_ack, tp_shm_send_ack };
static int  (* const send_fn[2])(idk_transport_t *, const idk_frame_header_t *, const int *, int) = { tp_socket_send, tp_shm_send };
static int  (* const wait_ack_fn[2])(idk_transport_t *, idk_ack_msg_t *, int) = { tp_socket_wait_ack, tp_shm_wait_ack };
static int  (* const send_request_fn[2])(idk_transport_t *, const idk_request_msg_t *) = { tp_socket_send_request, tp_shm_send_request };
static int  (* const recv_request_fn[2])(idk_transport_t *, idk_request_msg_t *, int) = { tp_socket_recv_request, tp_shm_recv_request };

#define B(tp) ((tp)->backend)

/* ── Public API — dispatches to backend ─────────────────────────────── */

int idk_tp_init(idk_transport_t *tp, idk_tp_role_t role, const char *name) {
    tp->role = role;
    tp->ready = false;
    tp->backend = (uint8_t)resolve_backend();
    memset(tp->_rsv, 0, sizeof(tp->_rsv));
    return init_fn[B(tp)](tp, name);
}

void idk_tp_destroy(idk_transport_t *tp) {
    destroy_fn[B(tp)](tp);
}

int idk_tp_accept(idk_transport_t *tp) {
    return accept_fn[B(tp)](tp);
}

int idk_tp_poll(idk_transport_t *tp) {
    return poll_fn[B(tp)](tp);
}

int idk_tp_recv(idk_transport_t *tp, idk_frame_header_t *hdr,
                int fds[4], int *nfd) {
    return recv_fn[B(tp)](tp, hdr, fds, nfd);
}

void idk_tp_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack) {
    send_ack_fn[B(tp)](tp, ack);
}

int idk_tp_send(idk_transport_t *tp, const idk_frame_header_t *hdr,
                const int *fds, int nfd) {
    return send_fn[B(tp)](tp, hdr, fds, nfd);
}

int idk_tp_wait_ack(idk_transport_t *tp, idk_ack_msg_t *ack, int timeout_ms) {
    return wait_ack_fn[B(tp)](tp, ack, timeout_ms);
}

int idk_tp_send_request(idk_transport_t *tp, const idk_request_msg_t *req) {
    return send_request_fn[B(tp)](tp, req);
}

int idk_tp_recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms) {
    return recv_request_fn[B(tp)](tp, req, timeout_ms);
}
