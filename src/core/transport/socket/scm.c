#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "internal.h"

static void close_received_fds(int fds[4], int *nfd) {
  if (!fds || !nfd)
    return;
  for (int i = 0; i < *nfd; i++) {
    if (fds[i] >= 0) {
      close(fds[i]);
      fds[i] = -1;
    }
  }
  *nfd = 0;
}

int scm_recv_fds(int fd, void *buf, size_t len, int fds[4], int *nfd) {
  if (fd < 0 || !buf || !fds || !nfd) {
    errno = EINVAL;
    return -1;
  }

  for (int i = 0; i < 4; i++)
    fds[i] = -1;
  *nfd = 0;

  char ctrl_buf[CMSG_SPACE(sizeof(int) * 4)] = {0};
  struct iovec iov = {.iov_base = buf, .iov_len = len};
  struct msghdr msgh = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = ctrl_buf,
      .msg_controllen = sizeof(ctrl_buf),
  };

  ssize_t n = recvmsg(fd, &msgh, MSG_DONTWAIT);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    return -1;
  }
  if (n == 0)
    return -1;

  bool control_truncated = (msgh.msg_flags & MSG_CTRUNC) != 0;
  bool malformed = false;
  bool has_rights = false;
  int received = 0;
  char *ctrl_end = ctrl_buf + sizeof(ctrl_buf);
  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh); cmsg && !malformed; cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
    size_t remaining = (size_t)(ctrl_end - (char *)cmsg);
    if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg->cmsg_len > remaining) {
      malformed = true;
      break;
    }
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
    if (data_len % sizeof(int) != 0 || data_len / sizeof(int) > (size_t)(4 - received)) {
      malformed = true;
      break;
    }
    memcpy(fds + received, CMSG_DATA(cmsg), data_len);
    received += (int)(data_len / sizeof(int));
    has_rights = true;
  }

  *nfd = received;
  if (control_truncated || malformed || !has_rights || (size_t)n != len) {
    close_received_fds(fds, nfd);
    errno = (control_truncated || malformed) ? EMSGSIZE : EPROTO;
    return -1;
  }
  return 1;
}

ssize_t scm_send_fds(int fd, const void *buf, size_t len, const int *fds, int nfd) {
  if (fd < 0 || !buf || !fds || nfd < 1 || nfd > 4) {
    errno = EINVAL;
    return -1;
  }

  struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
  char ctrl_buf[CMSG_SPACE(sizeof(int) * 4)] = {0};
  struct msghdr msgh = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = ctrl_buf,
      .msg_controllen = sizeof(ctrl_buf),
  };

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
  memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfd);
  msgh.msg_controllen = cmsg->cmsg_len;

  return sendmsg(fd, &msgh, MSG_DONTWAIT | MSG_NOSIGNAL);
}
