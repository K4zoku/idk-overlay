#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/transport.h"

int tp_socket_send_input(idk_transport_t *tp, const idk_input_event_t *ev) {
  if (!tp->ready || !ev) {
    errno = EINVAL;
    return -1;
  }
  if (tp->_client_fd < 0) {
    errno = ENOTCONN;
    return -1;
  }

  ssize_t n = send(tp->_client_fd, ev, sizeof(*ev), MSG_NOSIGNAL | MSG_DONTWAIT);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return -1;
    }
    if (errno == EPIPE || errno == ECONNRESET || errno == ESHUTDOWN || errno == ECONNABORTED || errno == EBADF) {
      tp->ready = false;
      close(tp->_client_fd);
      tp->_client_fd = -1;
    }
    return -1;
  }
  return ((size_t)n == sizeof(*ev)) ? 0 : -1;
}

int tp_socket_recv_input(idk_transport_t *tp, idk_input_event_t *ev) {
  if (!ev) {
    errno = EINVAL;
    return -1;
  }
  if (tp->_client_fd < 0) {
    errno = ENOTCONN;
    return -1;
  }

  ssize_t n;
  do {
    n = recv(tp->_client_fd, ev, sizeof(*ev), MSG_DONTWAIT);
  } while (n < 0 && errno == EINTR);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    if (errno == EPIPE || errno == ECONNRESET) {
      tp->ready = false;
      close(tp->_client_fd);
      tp->_client_fd = -1;
      return -1;
    }
    return -1;
  }
  if (n == 0) {
    tp->ready = false;
    close(tp->_client_fd);
    tp->_client_fd = -1;
    return -1;
  }
  if ((size_t)n != sizeof(*ev)) {
    return 0;
  }
  return 1;
}
