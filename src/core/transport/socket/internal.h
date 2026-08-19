#ifndef IDK_TP_SOCKET_INTERNAL_H
#define IDK_TP_SOCKET_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "core/transport.h"

/* _rsv[56] layout: [0]=state, [4]=connect_retries, [8..47]=path */
#define TP_S_STATE(rsv) (*(int *)(rsv))
#define TP_S_CONNECT_RETRIES(rsv) (*(int *)((rsv) + 4))
#define TP_S_ABSTRACT(rsv) ((rsv)[7])
#define TP_STATE_INIT 0
#define TP_STATE_LISTEN 1
#define TP_STATE_BOUND 2
#define TP_STATE_READY 3
#define TP_S_PATH_CAP 40

/* Internal helpers: not part of the public symbol surface. */
#define IDK_INTERNAL __attribute__((visibility("hidden")))

IDK_INTERNAL int set_nonblock(int fd);
IDK_INTERNAL bool is_abstract(const char *name);
IDK_INTERNAL socklen_t abstract_addrlen(const char *name);

IDK_INTERNAL int consumer_init(idk_transport_t *tp, const char *path);
IDK_INTERNAL int producer_init(idk_transport_t *tp, const char *path);

IDK_INTERNAL void tp_socket_disconnect_client(idk_transport_t *tp);
IDK_INTERNAL int scm_recv_fds(int fd, void *buf, size_t len, int fds[4], int *nfd);
IDK_INTERNAL ssize_t scm_send_fds(int fd, const void *buf, size_t len, const int *fds, int nfd);

#endif
