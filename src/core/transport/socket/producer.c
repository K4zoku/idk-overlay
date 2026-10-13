#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/log.h"
#include "internal.h"

static uint32_t socket_local_features(void) {
  uint32_t features = IDK_SOCKET_FEATURES_DEFAULT;
  const char *no_dmabuf = getenv("IDK_NO_DMABUF");
  if (no_dmabuf && no_dmabuf[0] && no_dmabuf[0] != '0')
    features &= ~IDK_SOCKET_FEATURE_DMABUF;
  return features;
}

static int begin_producer_handshake(idk_transport_t *tp, int fd) {
  if (!TP_S_SEQPACKET(tp->_rsv))
    return 0;
  uint64_t session_generation = socket_v2_new_session_generation();
  tp_socket_set_session_generation(tp, session_generation);
  tp_socket_set_next_message_id(tp, 2);
  const idk_socket_v2_identity_t identity = {
      .session_generation = session_generation,
      .message_id = 1,
  };
  if (socket_v2_send_hello(fd, IDK_SOCKET_FEATURES_DEFAULT, &identity) < 0)
    return -1;
  TP_S_STATE(tp->_rsv) = TP_STATE_HANDSHAKE;
  tp->ready = false;
  return 0;
}

int producer_init(idk_transport_t *tp, const char *path) {
  int fd = socket(AF_UNIX, TP_SOCKET_TYPE(tp), 0);
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
  TP_S_STATE(tp->_rsv) = TP_S_SEQPACKET(tp->_rsv) ? TP_STATE_HANDSHAKE : TP_STATE_READY;
  TP_S_QUEUED_REQ(tp->_rsv) = 0;
  TP_S_CONNECT_RETRIES(tp->_rsv) = 30;
  TP_S_ABSTRACT(tp->_rsv) = abstr ? 1 : 0;
  size_t cap = TP_S_PATH_CAP - (abstr ? 0 : 1);
  size_t cplen = name_len < cap ? name_len : cap;
  memcpy(tp->_rsv + 8, path, cplen);
  if (!abstr)
    tp->_rsv[8 + cplen] = '\0';
  tp->ready = !TP_S_SEQPACKET(tp->_rsv);
  if (begin_producer_handshake(tp, fd) < 0) {
    close(fd);
    tp->_client_fd = -1;
    return -1;
  }
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
      int fd = socket(AF_UNIX, TP_SOCKET_TYPE(tp), 0);
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
        tp->ready = !TP_S_SEQPACKET(tp->_rsv);
        TP_S_STATE(tp->_rsv) = TP_S_SEQPACKET(tp->_rsv) ? TP_STATE_HANDSHAKE : TP_STATE_READY;
        TP_S_QUEUED_REQ(tp->_rsv) = 0;
        TP_S_CONNECT_RETRIES(tp->_rsv) = 30;
        if (begin_producer_handshake(tp, fd) < 0) {
          close(fd);
          tp->_client_fd = -1;
          return -1;
        }
        IDK_LOG("tp", "socket: connected to %s%s (fd=%d)\n", abstr ? "\\0" : "", abstr ? path + 1 : path, fd);
        return 0;
      }
      close(fd);
      usleep(100000);
      return 0;
    }
    return -1;
  }
  if (TP_S_SEQPACKET(tp->_rsv) && !tp->ready) {
    uint32_t features = 0;
    uint64_t session_generation = tp_socket_session_generation(tp);
    uint64_t next_message_id = tp_socket_next_message_id(tp);
    int handshake = tp->role == IDK_TP_CONSUMER
                        ? socket_v2_server_handshake(tp->_client_fd, socket_local_features(), &features,
                                                     &session_generation, &next_message_id)
                        : socket_v2_client_handshake(tp->_client_fd, session_generation, &features, &next_message_id);
    if (handshake < 0) {
      tp_socket_disconnect_client(tp);
      return -1;
    }
    if (handshake == 1) {
      tp_socket_set_session_generation(tp, session_generation);
      tp_socket_set_next_message_id(tp, next_message_id);
      tp_socket_set_features(tp, features);
      TP_S_STATE(tp->_rsv) = TP_STATE_READY;
      tp->ready = true;
      IDK_LOG("tp", "socket: negotiated seqpacket features=0x%x\n", features);
      return 1;
    }
    return 0;
  }
  struct pollfd pfd = {.fd = tp->_client_fd, .events = POLLIN};
  int rc = poll(&pfd, 1, 0);
  if (rc < 0)
    return -1;
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
    tp_socket_disconnect_client(tp);
    return -1;
  }
  return (pfd.revents & POLLIN) ? 1 : 0;
}
