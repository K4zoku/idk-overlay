#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/log.h"
#include "internal.h"

int producer_init(idk_transport_t *tp, const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  int rc;
  bool abstr = is_abstract(path);
  size_t name_len;
  socklen_t addrlen;
  if (abstr) {
    name_len = 1 + strlen(path + 1);
    if (name_len >= sizeof(addr.sun_path)) {
      close(fd);
      return -1;
    }
    memcpy(addr.sun_path, path, name_len);
    addrlen = abstract_addrlen(path);
  } else {
    name_len = strlen(path);
    if (name_len >= sizeof(addr.sun_path)) {
      close(fd);
      return -1;
    }
    memcpy(addr.sun_path, path, name_len + 1);
    addrlen = sizeof(addr);
  }
  rc = connect(fd, (struct sockaddr *)&addr, addrlen);
  if (rc < 0) {
    if (errno == ECONNREFUSED || errno == ENOENT) {
      close(fd);
      tp->_server_fd = -1;
      tp->_client_fd = -1;
      TP_S_STATE(tp->_rsv) = TP_STATE_INIT;
      TP_S_CONNECT_RETRIES(tp->_rsv) = 30;
      TP_S_ABSTRACT(tp->_rsv) = abstr ? 1 : 0;
      size_t cap = TP_S_PATH_CAP - (abstr ? 0 : 1);
      size_t cplen = name_len < cap ? name_len : cap;
      memcpy(tp->_rsv + 8, path, cplen);
      if (!abstr)
        tp->_rsv[8 + cplen] = '\0';
      tp->ready = false;
      IDK_LOG("tp", "socket: producer defer connect to %s%s\n", abstr ? "\\0" : "", abstr ? path + 1 : path);
      return 0;
    }
    close(fd);
    return -1;
  }

  tp->_server_fd = -1;
  tp->_client_fd = fd;
  TP_S_STATE(tp->_rsv) = TP_STATE_READY;
  tp->ready = true;
  IDK_LOG("tp", "socket: connected to %s (fd=%d)\n", path, fd);
  return 0;
}

int tp_socket_poll(idk_transport_t *tp) {
  if (tp->_client_fd < 0) {
    if (tp->role == IDK_TP_PRODUCER && !tp->ready) {
      int *retries = &TP_S_CONNECT_RETRIES(tp->_rsv);
      if (*retries <= 0)
        return 0;
      (*retries)--;
      bool abstr = TP_S_ABSTRACT(tp->_rsv) != 0;
      const char *path = (const char *)(tp->_rsv + 8);
      if (!abstr && path[0] == '\0')
        return 0;
      int fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0)
        return 0;
      struct sockaddr_un addr = {.sun_family = AF_UNIX};
      socklen_t addrlen;
      if (abstr) {
        size_t len = 1 + strlen(path + 1);
        memcpy(addr.sun_path, path, len);
        addrlen = abstract_addrlen(path);
      } else {
        memcpy(addr.sun_path, path, strlen(path) + 1);
        addrlen = sizeof(addr);
      }
      if (connect(fd, (struct sockaddr *)&addr, addrlen) == 0) {
        tp->_client_fd = fd;
        tp->ready = true;
        IDK_LOG("tp", "socket: connected to %s%s (fd=%d)\n", abstr ? "\\0" : "", abstr ? path + 1 : path, fd);
        return 0;
      }
      close(fd);
      usleep(100000);
      return 0;
    }
    return -1;
  }

  struct pollfd pfd = {.fd = tp->_client_fd, .events = POLLIN};
  int rc = poll(&pfd, 1, 0);
  if (rc < 0)
    return -1;
  return (pfd.revents & POLLIN) ? 1 : 0;
}
