#ifndef IDK_TP_SOCKET_INTERNAL_H
#define IDK_TP_SOCKET_INTERNAL_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/transport.h"
#include "protocol.h"

/* _rsv[72]: [0..3]=state, [4..7]=connect retries, [8..47]=path,
 *            [48]=queued request, [49]=seqpacket, [56..63]=session generation,
 *            [64..71]=next message ID */
#define TP_S_STATE(rsv) (*(int *)(rsv))
#define TP_S_CONNECT_RETRIES(rsv) (*(int *)((rsv) + 4))
#define TP_S_ABSTRACT(rsv) ((rsv)[7])
#define TP_STATE_INIT 0
#define TP_STATE_LISTEN 1
#define TP_STATE_BOUND 2
#define TP_STATE_READY 3
#define TP_STATE_HANDSHAKE 4
#define TP_S_PATH_CAP 40
#define TP_S_QUEUED_REQ(rsv) ((rsv)[48])
#define TP_S_REQUEST_TAG 0x80u
#define TP_S_SEQPACKET(rsv) ((rsv)[49])

static inline uint64_t tp_socket_session_generation(const idk_transport_t *tp) {
  uint64_t value = 0;
  memcpy(&value, tp->_rsv + 56, sizeof(value));
  return value;
}

static inline void tp_socket_set_session_generation(idk_transport_t *tp, uint64_t value) {
  memcpy(tp->_rsv + 56, &value, sizeof(value));
}

static inline uint64_t tp_socket_next_message_id(const idk_transport_t *tp) {
  uint64_t value = 0;
  memcpy(&value, tp->_rsv + 64, sizeof(value));
  return value;
}

static inline void tp_socket_set_next_message_id(idk_transport_t *tp, uint64_t value) {
  memcpy(tp->_rsv + 64, &value, sizeof(value));
}

static inline int tp_socket_v2_send(idk_transport_t *tp, uint16_t type, const void *payload, size_t payload_bytes,
                                    const int *fds, int nfd) {
  uint64_t session_generation = tp_socket_session_generation(tp);
  uint64_t message_id = tp_socket_next_message_id(tp);
  if (session_generation == 0 || message_id == 0 || message_id == UINT64_MAX) {
    errno = EOVERFLOW;
    return -1;
  }
  tp_socket_set_next_message_id(tp, message_id + 1);
  const idk_socket_v2_identity_t identity = {
      .session_generation = session_generation,
      .message_id = message_id,
  };
  return socket_v2_send(tp->_client_fd, type, payload, payload_bytes, &identity, fds, nfd);
}

static inline int tp_socket_v2_recv(idk_transport_t *tp, uint16_t expected_type, void *payload, size_t payload_capacity,
                                    int fds[4], int *nfd) {
  idk_socket_v2_identity_t identity = {0};
  int rc = socket_v2_recv(tp->_client_fd, expected_type, payload, payload_capacity, &identity, fds, nfd);
  if (rc == 1 && identity.session_generation != tp_socket_session_generation(tp)) {
    for (int i = 0; i < *nfd; i++)
      close(fds[i]);
    *nfd = 0;
    errno = EBADMSG;
    return -1;
  }
  return rc;
}
#define TP_SOCKET_TYPE(tp) (TP_S_SEQPACKET((tp)->_rsv) ? SOCK_SEQPACKET : SOCK_STREAM)
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

static inline bool tp_socket_recv_fatal(ssize_t n) {
  return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

static inline void tp_socket_disconnect_on_recv_error(idk_transport_t *tp, ssize_t n) {
  if (tp_socket_recv_fatal(n))
    tp_socket_disconnect_client(tp);
}

#endif
