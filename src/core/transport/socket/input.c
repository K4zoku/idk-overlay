#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/transport.h"

static int cursor_valid(const idk_cursor_update_t *cursor, size_t capacity) {
  if (!cursor || cursor->magic != IDK_CURSOR_MAGIC || cursor->version != IDK_CURSOR_VERSION || cursor->visible > 1 ||
      cursor->data_size > IDK_CURSOR_MAX_BYTES || cursor->data_size > capacity)
    return 0;
  if (!cursor->visible)
    return cursor->data_size == 0;
  if (cursor->shape == IDK_CURSOR_CUSTOM)
    return cursor->width > 0 && cursor->width <= IDK_CURSOR_MAX_DIM && cursor->height > 0 &&
           cursor->height <= IDK_CURSOR_MAX_DIM && cursor->data_size == (uint32_t)cursor->width * cursor->height * 4u;
  return cursor->shape >= IDK_CURSOR_DEFAULT && cursor->shape <= IDK_CURSOR_ALL_RESIZE && cursor->width == 0 &&
         cursor->height == 0 && cursor->data_size == 0;
}

static int send_all(int fd, const void *data, size_t size) {
  const uint8_t *p = data;
  while (size > 0) {
    ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0) {
      errno = EPIPE;
      return -1;
    }
    p += (size_t)n;
    size -= (size_t)n;
  }
  return 0;
}

static int recv_all_nonblock(int fd, void *data, size_t size) {
  uint8_t *p = data;
  while (size > 0) {
    ssize_t n = recv(fd, p, size, MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0) {
      errno = ECONNRESET;
      return -1;
    }
    p += (size_t)n;
    size -= (size_t)n;
  }
  return 0;
}

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

int tp_socket_send_cursor(idk_transport_t *tp, const idk_cursor_update_t *cursor, const uint8_t *pixels) {
  if (!tp || !tp->ready || tp->_client_fd < 0 || !cursor || (cursor->data_size > 0 && !pixels) ||
      !cursor_valid(cursor, IDK_CURSOR_MAX_BYTES)) {
    errno = EINVAL;
    return -1;
  }
  if (send_all(tp->_client_fd, cursor, sizeof(*cursor)) != 0 ||
      (cursor->data_size > 0 && send_all(tp->_client_fd, pixels, cursor->data_size) != 0))
    return -1;
  return 0;
}

int tp_socket_recv_cursor(idk_transport_t *tp, idk_cursor_update_t *cursor, uint8_t *pixels, size_t capacity) {
  if (!tp || tp->_client_fd < 0 || !cursor || capacity > UINT_MAX) {
    errno = EINVAL;
    return -1;
  }
  idk_cursor_update_t peek;
  ssize_t n = recv(tp->_client_fd, &peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);
  if (n < 0)
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  if (n == 0) {
    errno = ECONNRESET;
    return -1;
  }
  if ((size_t)n < sizeof(peek))
    return 0;
  if (!cursor_valid(&peek, capacity) || (peek.data_size > 0 && !pixels)) {
    recv(tp->_client_fd, cursor, sizeof(*cursor), MSG_DONTWAIT);
    errno = EBADMSG;
    return -1;
  }
  int available = 0;
  if (ioctl(tp->_client_fd, FIONREAD, &available) != 0)
    return -1;
  size_t total = sizeof(peek) + peek.data_size;
  if (available < 0 || (size_t)available < total)
    return 0;
  if (recv_all_nonblock(tp->_client_fd, cursor, sizeof(*cursor)) != 0 ||
      (cursor->data_size > 0 && recv_all_nonblock(tp->_client_fd, pixels, cursor->data_size) != 0))
    return -1;
  return 1;
}
