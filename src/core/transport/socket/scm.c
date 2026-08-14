#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include "internal.h"

int scm_recv_fds(int fd, void *buf, size_t len, int fds[4], int *nfd) {
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
  if ((size_t)n < len)
    return -1;

  *nfd = 0;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&msgh); c; c = CMSG_NXTHDR(&msgh, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int flen = (int)(c->cmsg_len - CMSG_LEN(0)) / (int)sizeof(int);
      if (flen > 4)
        flen = 4;
      memcpy(fds, CMSG_DATA(c), (size_t)flen * sizeof(int));
      *nfd = flen;
      break;
    }
  }

  return 1;
}

ssize_t scm_send_fds(int fd, const void *buf, size_t len, const int *fds, int nfd) {
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
