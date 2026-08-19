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
  int rc = poll(&pfd, 1, timeout_ms);
  if (rc <= 0)
    return -1;
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
    tp_socket_disconnect_client(tp);
    return -1;
  }
  if (!(pfd.revents & POLLIN))
    return -1;

  size_t total = 0;
  while (total < sizeof(*ack)) {
    ssize_t n = read(tp->_client_fd, (char *)ack + total, sizeof(*ack) - total);
    if (n <= 0) {
      tp_socket_disconnect_client(tp);
      return -1;
    }
    total += (size_t)n;
  }
  return 0;
}
