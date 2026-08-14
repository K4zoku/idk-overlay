/* Input event IPC for idk-overlay.
 *
 * Implements input event send/recv (16-byte event, no fd passing) over
 * a separate Unix domain socket.  Frame transfer now uses the transport
 * abstraction (tp_socket.c / tp_shm.c).
 *
 * See include/public/idk_ipc.h for the wire format.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/log.h"
#include "public/idk_ipc.h"

int idk_ipc_send_input(int socket_fd, const idk_input_event_t *ev) {
  if (socket_fd < 0 || !ev) {
    errno = EINVAL;
    return -1;
  }

  ssize_t n = send(socket_fd, ev, sizeof(*ev), MSG_NOSIGNAL | MSG_DONTWAIT);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return -1;
    }
    IDK_ERR("ipc", "send_input failed: %s\n", strerror(errno));
    return -1;
  }
  if ((size_t)n != sizeof(*ev)) {
    return -1;
  }
  return 0;
}

int idk_ipc_recv_input(int socket_fd, idk_input_event_t *ev, int flags) {
  if (socket_fd < 0 || !ev) {
    errno = EINVAL;
    return -1;
  }

  size_t total = 0;
  while (total < sizeof(*ev)) {
    ssize_t n = recv(socket_fd, (char *)ev + total, sizeof(*ev) - total, flags);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0) {
      errno = ECONNRESET;
      return -1;
    }
    total += (size_t)n;
    flags = 0;
  }

  if (ev->type < IDK_INPUT_KEY || ev->type > IDK_INPUT_OVERLAY) {
    errno = EBADMSG;
    return -1;
  }

  return 0;
}
