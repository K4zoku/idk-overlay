#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "core/transport.h"
#include "internal.h"

int tp_socket_recv(idk_transport_t *tp, idk_frame_header_t *hdr, int fds[4], int *nfd) {
  if (tp->_client_fd < 0 || !hdr || !fds || !nfd) {
    errno = EINVAL;
    return -1;
  }
  return scm_recv_fds(tp->_client_fd, hdr, sizeof(*hdr), fds, nfd);
}

int tp_socket_drop_frame(idk_transport_t *tp) {
  idk_frame_header_t hdr;
  int fds[4], nfd = 0;
  int rc = tp_socket_recv(tp, &hdr, fds, &nfd);
  if (rc <= 0)
    return rc;
  for (int i = 0; i < nfd; i++)
    close(fds[i]);
  return 1;
}

int tp_socket_send(idk_transport_t *tp, const idk_frame_header_t *hdr, const int *fds, int nfd) {
  if (tp->_client_fd < 0 || !hdr || !fds || nfd < 1 || nfd > 4) {
    errno = EINVAL;
    return -1;
  }

  ssize_t n = scm_send_fds(tp->_client_fd, hdr, sizeof(*hdr), fds, nfd);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return -1;
    if (errno == EPIPE || errno == ECONNRESET || errno == ESHUTDOWN || errno == ECONNABORTED || errno == EBADF) {
      if (tp->ready) {
        tp->ready = false;
        close(tp->_client_fd);
        tp->_client_fd = -1;
      }
      return -1;
    }
    IDK_ERR("tp", "sendmsg failed: %s\n", strerror(errno));
    return -1;
  }
  if ((size_t)n != sizeof(*hdr))
    return -1;
  return 0;
}
