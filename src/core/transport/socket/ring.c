#include <errno.h>

#include "internal.h"

int tp_socket_send_control(idk_transport_t *tp, uint16_t type, const void *payload, size_t payload_bytes,
                           const int *fds, int nfd) {
  if (!TP_S_SEQPACKET(tp->_rsv)) {
    errno = ENOTSUP;
    return -1;
  }
  uint64_t generation = tp_socket_session_generation(tp);
  uint64_t message_id = tp_socket_next_message_id(tp);
  if (generation == 0 || message_id == 0 || message_id == UINT64_MAX) {
    errno = EOVERFLOW;
    return -1;
  }
  idk_socket_v2_identity_t identity = {.session_generation = generation, .message_id = message_id};
  int rc = socket_v2_send(tp->_client_fd, type, payload, payload_bytes, &identity, fds, nfd);
  if (rc == 0)
    tp_socket_set_next_message_id(tp, message_id + 1);
  return rc;
}

int tp_socket_recv_control(idk_transport_t *tp, uint16_t type, void *payload, size_t payload_capacity, int fds[4], int *nfd) {
  if (!TP_S_SEQPACKET(tp->_rsv)) {
    errno = ENOTSUP;
    return -1;
  }
  return tp_socket_v2_recv(tp, type, payload, payload_capacity, fds, nfd);
}
