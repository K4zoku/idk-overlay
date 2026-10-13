#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "internal.h"

/* REQUEST and ACK share the producer-side SOCK_STREAM. Tag only REQUEST
 * on the socket wire; SHM transports retain the public 8-byte structure. */
int tp_socket_send_request(idk_transport_t *tp, const idk_request_msg_t *req) {
  if (tp->_client_fd < 0 || !req || req->type > IDK_REQUEST_SHUTDOWN) {
    errno = EINVAL;
    return -1;
  }
  if (TP_S_SEQPACKET(tp->_rsv)) {
    if (tp_socket_v2_send(tp, IDK_SOCKET_V2_REQUEST, req, sizeof(*req), NULL, 0) < 0) {
      if (errno == EPIPE || errno == ECONNRESET || errno == ESHUTDOWN || errno == ECONNABORTED || errno == EBADF)
        tp_socket_disconnect_client(tp);
      return -1;
    }
    return 0;
  }
  idk_request_msg_t wire = *req;
  wire.type |= TP_S_REQUEST_TAG;
  ssize_t n = send(tp->_client_fd, &wire, sizeof(wire), MSG_NOSIGNAL);
  if (n < 0) {
    if (errno == EPIPE || errno == ECONNRESET || errno == ESHUTDOWN || errno == ECONNABORTED || errno == EBADF)
      tp_socket_disconnect_client(tp);
    return -1;
  }
  if ((size_t)n != sizeof(wire)) {
    tp_socket_disconnect_client(tp);
    return -1;
  }
  return 0;
}

/* Return one coalesced REQUEST already captured by wait_ack. Shutdown wins. */
static int take_queued(idk_transport_t *tp, idk_request_msg_t *req) {
  uint8_t queued = TP_S_QUEUED_REQ(tp->_rsv);
  if (queued == 0)
    return -1;
  TP_S_QUEUED_REQ(tp->_rsv) = 0;
  *req = (idk_request_msg_t){0};
  req->type = queued - 1;
  return 0;
}

static int recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms, bool preserve_ack) {
  if (tp->_client_fd < 0 || !req) {
    errno = EINVAL;
    return -1;
  }
  if (take_queued(tp, req) == 0)
    return 0;
  if (TP_S_SEQPACKET(tp->_rsv)) {
    for (int i = 0; i < 256; i++) {
      uint16_t type = 0;
      int peek = socket_v2_peek_type(tp->_client_fd, &type);
      if (peek <= 0) {
        tp_socket_disconnect_on_recv_error(tp, peek);
        return -1;
      }
      if (type == IDK_SOCKET_V2_REQUEST) {
        int fds[4];
        int nfd = 0;
        int rc = tp_socket_v2_recv(tp, IDK_SOCKET_V2_REQUEST, req, sizeof(*req), fds, &nfd);
        for (int j = 0; j < nfd; j++)
          close(fds[j]);
        if (rc != 1 || nfd != 0) {
          tp_socket_disconnect_on_recv_error(tp, rc);
          return -1;
        }
        if (req->type > IDK_REQUEST_SHUTDOWN)
          return -1;
        return 0;
      }
      if (type == IDK_SOCKET_V2_ACK) {
        if (preserve_ack) {
          errno = EAGAIN;
          return -1;
        }
        idk_ack_msg_t stale;
        int fds[4];
        int nfd = 0;
        int rc = tp_socket_v2_recv(tp, IDK_SOCKET_V2_ACK, &stale, sizeof(stale), fds, &nfd);
        for (int j = 0; j < nfd; j++)
          close(fds[j]);
        if (rc != 1 || nfd != 0)
          return -1;
        continue;
      }
      errno = EBADMSG;
      return -1;
    }
    errno = ELOOP;
    return -1;
  }

  struct pollfd pfd = {.fd = tp->_client_fd, .events = POLLIN};
  if (poll(&pfd, 1, timeout_ms) <= 0)
    return -1;
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
    tp_socket_disconnect_client(tp);
    return -1;
  }
  /* ACKs from a previous timed-out producer frame may remain in the stream.
   * Drop stale ACK records rather than decode the first eight bytes as REQUEST. */
  for (int i = 0; i < 256; ++i) {
    uint8_t first;
    ssize_t n = recv(tp->_client_fd, &first, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n <= 0)
      return -1;
    if (first & TP_S_REQUEST_TAG) {
      idk_request_msg_t wire;
      if (recv(tp->_client_fd, &wire, sizeof(wire), MSG_PEEK | MSG_DONTWAIT) != (ssize_t)sizeof(wire))
        return -1;
      if (recv(tp->_client_fd, &wire, sizeof(wire), MSG_DONTWAIT) != (ssize_t)sizeof(wire)) {
        tp_socket_disconnect_client(tp);
        return -1;
      }
      req->type = wire.type & ~TP_S_REQUEST_TAG;
      if (req->type > IDK_REQUEST_SHUTDOWN) {
        tp_socket_disconnect_client(tp);
        return -1;
      }
      return 0;
    }
    if (first > IDK_ACK_DISCARDED) {
      tp_socket_disconnect_client(tp);
      return -1;
    }
    if (preserve_ack) {
      errno = EAGAIN;
      return -1;
    }
    idk_ack_msg_t stale;
    if (recv(tp->_client_fd, &stale, sizeof(stale), MSG_PEEK | MSG_DONTWAIT) != (ssize_t)sizeof(stale))
      return -1;
    if (recv(tp->_client_fd, &stale, sizeof(stale), MSG_DONTWAIT) != (ssize_t)sizeof(stale)) {
      tp_socket_disconnect_client(tp);
      return -1;
    }
  }
  return -1;
}

int tp_socket_recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms) {
  return recv_request(tp, req, timeout_ms, false);
}

int tp_socket_recv_request_preserving_ack(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms) {
  return recv_request(tp, req, timeout_ms, true);
}
