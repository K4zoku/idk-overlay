#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include "internal.h"

ssize_t recv_full(int fd, void *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = read(fd, (char *)buf + total, len - total);
    if (n == 0)
      return 0;
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    total += (size_t)n;
  }
  return (ssize_t)total;
}

/* Returns 0 if the handshake is acceptable; prints a diagnostic and
 * returns -1 otherwise. */
int validate_handshake(const idk_cp_handshake_t *hs) {
  if (hs->identity != IDK_CP_ID_OVERLAY) {
    fprintf(stderr, "idk-broker: bad identity 0x%x\n", hs->identity);
    return -1;
  }
  if (hs->tp_socket[0] == '\0' || hs->input_socket[0] == '\0') {
    fprintf(stderr, "idk-broker: empty socket names\n");
    return -1;
  }
  return 0;
}
