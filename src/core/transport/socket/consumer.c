#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/log.h"
#include "internal.h"

int consumer_init(idk_transport_t *tp, const char *path) {
  if (is_abstract(path)) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      return -1;
    set_nonblock(fd);

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    memcpy(addr.sun_path, path, 1 + strlen(path + 1));
    socklen_t addrlen = abstract_addrlen(path);

    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0) {
      close(fd);
      return -1;
    }
    listen(fd, 4);

    tp->_server_fd = fd;
    tp->_client_fd = -1;
    TP_S_STATE(tp->_rsv) = TP_STATE_LISTEN;
    tp->ready = false;
    IDK_LOG("tp", "socket: listening on abstract \\0%s\n", path + 1);
    return 0;
  }

  struct stat st;
  if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
    int test = socket(AF_UNIX, SOCK_STREAM, 0);
    if (test >= 0) {
      struct sockaddr_un addr = {.sun_family = AF_UNIX};
      snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", path);
      if (connect(test, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(test);
        IDK_ERR("tp", "Another instance owns %s\n", path);
        return -1;
      }
      close(test);
    }
  }

  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  set_nonblock(fd);

  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", path);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  listen(fd, 4);

  tp->_server_fd = fd;
  tp->_client_fd = -1;
  TP_S_STATE(tp->_rsv) = TP_STATE_LISTEN;
  tp->ready = false;
  IDK_LOG("tp", "socket: listening on %s\n", path);
  return 0;
}

int tp_socket_accept(idk_transport_t *tp) {
  if (tp->_server_fd < 0)
    return -1;
  if (tp->_client_fd >= 0)
    return 1;

  int c = accept(tp->_server_fd, NULL, NULL);
  if (c < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  }
  set_nonblock(c);
  tp->_client_fd = c;
  tp->ready = true;
  IDK_LOG("tp", "socket: client connected (fd=%d)\n", c);
  return 1;
}
