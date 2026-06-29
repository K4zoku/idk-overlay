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

typedef struct idk_transport {
    idk_tp_role_t role;
    uint8_t       backend;      /* 0 = socket, 1 = SHM */
    bool          ready;

    /* backend-internal */
    int           _server_fd;
    int           _client_fd;
    uint8_t       _rsv[56];
} idk_transport_t;

#ifdef __cplusplus
static_assert(sizeof(idk_transport_t) == 72,
              "idk_transport_t must be 72 bytes");
#else
_Static_assert(sizeof(idk_transport_t) == 72,
               "idk_transport_t must be 72 bytes");
#endif

int  idk_tp_init(idk_transport_t *tp, idk_tp_role_t role, const char *name);
void idk_tp_destroy(idk_transport_t *tp);
void idk_tp_disconnect_client(idk_transport_t *tp);

int  idk_tp_accept(idk_transport_t *tp);
int  idk_tp_poll(idk_transport_t *tp);
int  idk_tp_recv(idk_transport_t *tp, idk_frame_header_t *hdr,
                 int fds[4], int *nfd);
void idk_tp_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack);
int idk_tp_send_request(idk_transport_t *tp, const idk_request_msg_t *req);
int idk_tp_recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms);

int  idk_tp_send(idk_transport_t *tp, const idk_frame_header_t *hdr,
                 const int *fds, int nfd);
int  idk_tp_wait_ack(idk_transport_t *tp, idk_ack_msg_t *ack, int timeout_ms);

int  idk_tp_send_input(idk_transport_t *tp, const idk_input_event_t *ev);
int  idk_tp_recv_input(idk_transport_t *tp, idk_input_event_t *ev);

#ifdef __cplusplus
}
#endif
