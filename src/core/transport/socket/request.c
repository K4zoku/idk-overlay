#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include "core/transport.h"

int tp_socket_send_request(idk_transport_t *tp, const idk_request_msg_t *req) {
  if (tp->_client_fd < 0 || !req) {
    errno = EINVAL;
    return -1;
  }
  ssize_t n = write(tp->_client_fd, req, sizeof(*req));
  if (n < 0) {
    if (errno == EPIPE || errno == ECONNRESET) {
      tp->ready = false;
      close(tp->_client_fd);
      tp->_client_fd = -1;
    }
    return -1;
  }
  return ((size_t)n == sizeof(*req)) ? 0 : -1;
}

int tp_socket_recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms) {
  if (tp->_client_fd < 0 || !req) {
    errno = EINVAL;
    return -1;
  }
  struct pollfd pfd = {.fd = tp->_client_fd, .events = POLLIN};
  if (poll(&pfd, 1, timeout_ms) <= 0)
    return -1;
  if (!(pfd.revents & POLLIN))
    return -1;
  ssize_t n;
  do {
    n = read(tp->_client_fd, req, sizeof(*req));
  } while (n < 0 && errno == EINTR);
  if (n <= 0)
    return -1;
  return ((size_t)n == sizeof(*req)) ? 0 : -1;
}
