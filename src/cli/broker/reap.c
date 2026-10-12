#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "internal.h"

int broker_pidfd_open(pid_t pid) {
#ifdef SYS_pidfd_open
  return (int)syscall(SYS_pidfd_open, pid, 0);
#else
  (void)pid;
  errno = ENOSYS;
  return -1;
#endif
}

int broker_pidfd_send_signal(int pidfd, int signal) {
#ifdef SYS_pidfd_send_signal
  return (int)syscall(SYS_pidfd_send_signal, pidfd, signal, NULL, 0);
#else
  (void)pidfd;
  (void)signal;
  errno = ENOSYS;
  return -1;
#endif
}

static int child_exited(pid_t pid, int pidfd, int timeout_ms) {
  if (pidfd >= 0) {
    struct pollfd pfd = {.fd = pidfd, .events = POLLIN, .revents = 0};
    int rc;
    do {
      rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    return rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR));
  }

  int status;
  if (waitpid(pid, &status, WNOHANG) == pid)
    return 1;
  return 0;
}

static void reap_child(pid_t pid, int pidfd) {
  if (pidfd >= 0) {
    siginfo_t info = {};
    if (waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED) == 0)
      return;
  }

  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    continue;
}

void reap_webview(pid_t pid, int pidfd) {
  if (pid <= 0)
    return;

  if (pidfd >= 0) {
    if (!child_exited(pid, pidfd, 0))
      (void)broker_pidfd_send_signal(pidfd, SIGTERM);
    if (!child_exited(pid, pidfd, 1000))
      (void)broker_pidfd_send_signal(pidfd, SIGKILL);
    reap_child(pid, pidfd);
    close(pidfd);
    return;
  }

  (void)kill(pid, SIGTERM);
  for (int i = 0; i < 20; i++) {
    if (child_exited(pid, -1, 0))
      return;
    usleep(50000);
  }
  (void)kill(pid, SIGKILL);
  reap_child(pid, -1);
}
