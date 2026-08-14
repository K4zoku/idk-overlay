#include <fcntl.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#include "internal.h"

int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return (fl < 0) ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

bool is_abstract(const char *name) { return name && name[0] == '\0'; }

socklen_t abstract_addrlen(const char *name) {
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name + 1));
}

int tp_socket_init(idk_transport_t *tp, const char *name) {
  idk_tp_role_t role = tp->role;
  uint8_t backend = tp->backend;
  memset(tp->_rsv, 0, sizeof(tp->_rsv));
  tp->_server_fd = -1;
  tp->_client_fd = -1;
  tp->ready = false;
  tp->role = role;
  tp->backend = backend;
  if (role == IDK_TP_CONSUMER)
    return consumer_init(tp, name);
  else
    return producer_init(tp, name);
}

void tp_socket_destroy(idk_transport_t *tp) {
  if (tp->_client_fd >= 0) {
    close(tp->_client_fd);
    tp->_client_fd = -1;
  }
  if (tp->_server_fd >= 0) {
    close(tp->_server_fd);
    tp->_server_fd = -1;
  }
  tp->ready = false;
}

void tp_socket_disconnect_client(idk_transport_t *tp) {
  if (tp->_client_fd >= 0) {
    close(tp->_client_fd);
    tp->_client_fd = -1;
  }
  tp->ready = false;
}
