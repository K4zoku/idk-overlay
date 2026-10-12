#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "internal.h"

void tp_socket_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack) {
  if (tp->_client_fd < 0 || !ack)
    return;
  ssize_t n = send(tp->_client_fd, ack, sizeof(*ack), MSG_NOSIGNAL);
  if (n < 0 &&
      (errno == EPIPE || errno == ECONNRESET || errno == ESHUTDOWN || errno == ECONNABORTED || errno == EBADF)) {
    tp_socket_disconnect_client(tp);
  } else if (n >= 0 && (size_t)n != sizeof(*ack)) {
    tp_socket_disconnect_client(tp);
  }
}

int tp_socket_wait_ack(idk_transport_t *tp, idk_ack_msg_t *ack, int timeout_ms) {
  if (tp->_client_fd < 0 || !ack) {
    errno = EINVAL;
    return -1;
  }
  struct pollfd pfd = {.fd = tp->_client_fd, .events = POLLIN};
  if (poll(&pfd, 1, timeout_ms) <= 0)
    return -1;
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
    tp_socket_disconnect_client(tp);
    return -1;
  }
  /* Demultiplex queued REQUESTs before ACK. Otherwise two 8-byte REQUESTs
   * may be mistaken for a 16-byte successful ACK on a stream socket. */
  for (int i = 0; i < 256; ++i) {
    uint8_t first;
    if (recv(tp->_client_fd, &first, 1, MSG_PEEK | MSG_DONTWAIT) != 1)
      return -1;
    if (first & TP_S_REQUEST_TAG) {
      idk_request_msg_t wire;
      if (recv(tp->_client_fd, &wire, sizeof(wire), MSG_PEEK | MSG_DONTWAIT) != (ssize_t)sizeof(wire))
        return -1;
      if (recv(tp->_client_fd, &wire, sizeof(wire), MSG_DONTWAIT) != (ssize_t)sizeof(wire)) {
        tp_socket_disconnect_client(tp);
        return -1;
      }
      uint8_t type = wire.type & ~TP_S_REQUEST_TAG;
      if (type > IDK_REQUEST_SHUTDOWN) {
        tp_socket_disconnect_client(tp);
        return -1;
      }
      /* 1 NEXT_FRAME, 2 SHUTDOWN. Coalesce, but never lose SHUTDOWN. */
      uint8_t *queued = &TP_S_QUEUED_REQ(tp->_rsv);
      if (type == IDK_REQUEST_SHUTDOWN || *queued == 0)
        *queued = type + 1;
      continue;
    }
    if (first > IDK_ACK_DISCARDED) {
      tp_socket_disconnect_client(tp);
      return -1;
    }
    if (recv(tp->_client_fd, ack, sizeof(*ack), MSG_PEEK | MSG_DONTWAIT) != (ssize_t)sizeof(*ack))
      return -1;
    if (recv(tp->_client_fd, ack, sizeof(*ack), MSG_DONTWAIT) != (ssize_t)sizeof(*ack)) {
      tp_socket_disconnect_client(tp);
      return -1;
    }
    return 0;
  }
  return -1;
}
