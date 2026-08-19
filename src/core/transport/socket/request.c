#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include "internal.h"

int tp_socket_send_request(idk_transport_t *tp, const idk_request_msg_t *req) {
  if (tp->_client_fd < 0 || !req) {
    errno = EINVAL;
    return -1;
  }
  ssize_t n = write(tp->_client_fd, req, sizeof(*req));
  if (n < 0) {
    if (errno == EPIPE || errno == ECONNRESET || errno == ESHUTDOWN || errno == ECONNABORTED || errno == EBADF)
      tp_socket_disconnect_client(tp);
    return -1;
  }
  if ((size_t)n != sizeof(*req)) {
    tp_socket_disconnect_client(tp);
    return -1;
  }
  return 0;
}

int tp_socket_recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms) {
  if (tp->_client_fd < 0 || !req) {
    errno = EINVAL;
    return -1;
  }
  struct pollfd pfd = {.fd = tp->_client_fd, .events = POLLIN};
  int rc = poll(&pfd, 1, timeout_ms);
  if (rc <= 0)
    return -1;
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
    tp_socket_disconnect_client(tp);
    return -1;
  }
  if (!(pfd.revents & POLLIN))
    return -1;
  ssize_t n;
  do {
    n = read(tp->_client_fd, req, sizeof(*req));
  } while (n < 0 && errno == EINTR);
  if (n <= 0) {
    tp_socket_disconnect_client(tp);
    return -1;
  }
  if ((size_t)n != sizeof(*req)) {
    tp_socket_disconnect_client(tp);
    return -1;
  }
  return 0;
}
