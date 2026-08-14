#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "core/log.h"

#include "internal.h"

int shm_steal_fd(idk_transport_t *tp, void *ptr, int target_fd, int i, int *fds) {
  if (i == 0 && target_fd == TP_SH_CACHED_WV_FD(tp->_rsv) && TP_SH_CACHED_FD(tp->_rsv) >= 0) {
    int fd = dup(TP_SH_CACHED_FD(tp->_rsv));
    if (fd < 0) {
      for (int j = 0; j < i; j++)
        close(fds[j]);
      return -1;
    }
    return fd;
  }

  if (tp->_client_fd < 0) {
    int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
    int pidfd = (int)syscall(__NR_pidfd_open, prod_pid, 0);
    if (pidfd < 0) {
      IDK_ERR("tp", "shm: pidfd_open(%d) failed: %s\n", prod_pid, strerror(errno));
      return -1;
    }
    tp->_client_fd = pidfd;
  }

  int stolen = (int)syscall(__NR_pidfd_getfd, tp->_client_fd, target_fd, 0);
  if (stolen < 0) {
    IDK_ERR("tp", "shm: pidfd_getfd(%d) failed: %s\n", target_fd, strerror(errno));
    for (int j = 0; j < i; j++)
      close(fds[j]);
    return -1;
  }

  if (i == 0) {
    if (TP_SH_CACHED_FD(tp->_rsv) >= 0)
      close(TP_SH_CACHED_FD(tp->_rsv));
    TP_SH_CACHED_FD(tp->_rsv) = stolen;
    TP_SH_CACHED_WV_FD(tp->_rsv) = target_fd;
    int fd = dup(stolen);
    if (fd < 0) {
      for (int j = 0; j < i; j++)
        close(fds[j]);
      return -1;
    }
    return fd;
  }
  return stolen;
}
