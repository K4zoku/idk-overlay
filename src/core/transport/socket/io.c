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
  if (TP_S_SEQPACKET(tp->_rsv)) {
    int rc = socket_v2_recv(tp->_client_fd, IDK_SOCKET_V2_FRAME, hdr, sizeof(*hdr), fds, nfd);
    if (rc == 1 && hdr->nfd != 0 && hdr->nfd != *nfd) {
      for (int i = 0; i < *nfd; i++)
        close(fds[i]);
      *nfd = 0;
      errno = EBADMSG;
      return -1;
    }
    return rc;
  }
  return scm_recv_fds(tp->_client_fd, hdr, sizeof(*hdr), fds, nfd);
}


int tp_socket_drop_frame(idk_transport_t *tp) {
  idk_frame_header_t hdr;
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  int rc = tp_socket_recv(tp, &hdr, fds, &nfd);
  if (rc <= 0)
    return rc;
  for (int i = 0; i < 4; i++) {
    if (fds[i] >= 0)
      close(fds[i]);
    fds[i] = -1;
  }
  return 1;
}

int tp_socket_send(idk_transport_t *tp, const idk_frame_header_t *hdr, const int *fds, int nfd) {
  if (tp->_client_fd < 0 || !hdr || !fds || nfd < 1 || nfd > 4) {
    errno = EINVAL;
    return -1;
  }
  int rc = TP_S_SEQPACKET(tp->_rsv)
               ? socket_v2_send(tp->_client_fd, IDK_SOCKET_V2_FRAME, hdr, sizeof(*hdr), fds, nfd)
               : (scm_send_fds(tp->_client_fd, hdr, sizeof(*hdr), fds, nfd) == (ssize_t)sizeof(*hdr) ? 0 : -1);
  if (rc < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return -1;
    if (errno == EPIPE || errno == ECONNRESET || errno == ESHUTDOWN || errno == ECONNABORTED || errno == EBADF)
      tp_socket_disconnect_client(tp);
    return -1;
  }
  return 0;
}
