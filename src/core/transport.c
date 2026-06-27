/* Transport abstraction: routes idk_transport API calls to SHM or socket backend. */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/transport.h"

/* Backend declarations */
extern int  tp_socket_init(idk_transport_t *tp, const char *name);
extern void tp_socket_destroy(idk_transport_t *tp);
extern void tp_socket_disconnect_client(idk_transport_t *tp);
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
extern void tp_shm_disconnect_client(idk_transport_t *tp);
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

static int resolve_backend(void) {
    const char *env = getenv("IDK_TP_BACKEND");
    if (env && strcasecmp(env, "shm") == 0)
        return IDK_TP_SHM;
    return IDK_TP_SOCKET;
}

typedef struct {
    int  (*init)(idk_transport_t *, const char *);
    void (*destroy)(idk_transport_t *);
    void (*disconnect_client)(idk_transport_t *);
    int  (*accept)(idk_transport_t *);
    int  (*poll)(idk_transport_t *);
    int  (*recv)(idk_transport_t *, idk_frame_header_t *, int[4], int *);
    void (*send_ack)(idk_transport_t *, const idk_ack_msg_t *);
    int  (*send)(idk_transport_t *, const idk_frame_header_t *, const int *, int);
    int  (*wait_ack)(idk_transport_t *, idk_ack_msg_t *, int);
    int  (*send_request)(idk_transport_t *, const idk_request_msg_t *);
    int  (*recv_request)(idk_transport_t *, idk_request_msg_t *, int);
} idk_tp_backend_t;

static const idk_tp_backend_t tp_backends[2] = {
    [IDK_TP_SOCKET] = {
        .init = tp_socket_init, .destroy = tp_socket_destroy,
        .disconnect_client = tp_socket_disconnect_client,
        .accept = tp_socket_accept, .poll = tp_socket_poll,
        .recv = tp_socket_recv, .send_ack = tp_socket_send_ack,
        .send = tp_socket_send, .wait_ack = tp_socket_wait_ack,
        .send_request = tp_socket_send_request, .recv_request = tp_socket_recv_request,
    },
    [IDK_TP_SHM] = {
        .init = tp_shm_init, .destroy = tp_shm_destroy,
        .disconnect_client = tp_shm_disconnect_client,
        .accept = tp_shm_accept, .poll = tp_shm_poll,
        .recv = tp_shm_recv, .send_ack = tp_shm_send_ack,
        .send = tp_shm_send, .wait_ack = tp_shm_wait_ack,
        .send_request = tp_shm_send_request, .recv_request = tp_shm_recv_request,
    },
};

int idk_tp_init(idk_transport_t *tp, idk_tp_role_t role, const char *name) {
    tp->role = role;
    tp->ready = false;
    tp->backend = (uint8_t)resolve_backend();
    memset(tp->_rsv, 0, sizeof(tp->_rsv));
    return tp_backends[tp->backend].init(tp, name);
}

void idk_tp_destroy(idk_transport_t *tp) {
    tp_backends[tp->backend].destroy(tp);
}

void idk_tp_disconnect_client(idk_transport_t *tp) {
    tp_backends[tp->backend].disconnect_client(tp);
}

int idk_tp_accept(idk_transport_t *tp) {
    return tp_backends[tp->backend].accept(tp);
}

int idk_tp_poll(idk_transport_t *tp) {
    return tp_backends[tp->backend].poll(tp);
}

int idk_tp_recv(idk_transport_t *tp, idk_frame_header_t *hdr,
                int fds[4], int *nfd) {
    return tp_backends[tp->backend].recv(tp, hdr, fds, nfd);
}

void idk_tp_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack) {
    tp_backends[tp->backend].send_ack(tp, ack);
}

int idk_tp_send(idk_transport_t *tp, const idk_frame_header_t *hdr,
                const int *fds, int nfd) {
    return tp_backends[tp->backend].send(tp, hdr, fds, nfd);
}

int idk_tp_wait_ack(idk_transport_t *tp, idk_ack_msg_t *ack, int timeout_ms) {
    return tp_backends[tp->backend].wait_ack(tp, ack, timeout_ms);
}

int idk_tp_send_request(idk_transport_t *tp, const idk_request_msg_t *req) {
    return tp_backends[tp->backend].send_request(tp, req);
}

int idk_tp_recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms) {
    return tp_backends[tp->backend].recv_request(tp, req, timeout_ms);
}
