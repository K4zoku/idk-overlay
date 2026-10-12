#include "protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#pragma pack(push, 1)
typedef struct socket_v2_header {
  uint32_t magic;
  uint16_t major;
  uint16_t minor;
  uint16_t type;
  uint16_t flags;
  uint32_t payload_bytes;
  uint64_t session_generation;
  uint64_t message_id;
} socket_v2_header_t;
#pragma pack(pop)

_Static_assert(sizeof(socket_v2_header_t) == 32, "socket_v2_header_t must be 32 bytes");

static void close_fds(int fds[4], int *nfd) {
  if (!fds || !nfd)
    return;
  for (int i = 0; i < *nfd && i < 4; i++) {
    if (fds[i] >= 0)
      close(fds[i]);
    fds[i] = -1;
  }
  *nfd = 0;
}

static int collect_fds(struct msghdr *msg, int fds[4], int *nfd) {
  int received = 0;
  bool malformed = false;
  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(0)) {
      malformed = true;
      continue;
    }
    size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
    if (bytes % sizeof(int) != 0) {
      malformed = true;
      size_t count = bytes / sizeof(int);
      for (size_t i = 0; i < count; i++)
        close(((int *)CMSG_DATA(cmsg))[i]);
      continue;
    }
    size_t count = bytes / sizeof(int);
    if (count > 4 - (size_t)received) {
      malformed = true;
      for (size_t i = 0; i < count; i++)
        close(((int *)CMSG_DATA(cmsg))[i]);
      continue;
    }
    memcpy(fds + received, CMSG_DATA(cmsg), bytes);
    received += (int)count;
  }
  *nfd = received;
  return malformed ? -1 : 0;
}

int socket_v2_send(int fd, uint16_t type, const void *payload, size_t payload_bytes, const int *fds, int nfd) {
  if (fd < 0 || !payload || payload_bytes > IDK_SOCKET_V2_MAX_PAYLOAD || nfd < 0 || nfd > 4 ||
      (nfd > 0 && !fds)) {
    errno = EINVAL;
    return -1;
  }

  socket_v2_header_t header = {
      .magic = IDK_SOCKET_V2_MAGIC,
      .major = IDK_SOCKET_V2_MAJOR,
      .minor = 0,
      .type = type,
      .flags = 0,
      .payload_bytes = (uint32_t)payload_bytes,
      .session_generation = 0,
      .message_id = 0,
  };
  char control[CMSG_SPACE(sizeof(int) * 4)] = {0};
  struct iovec iov[2] = {{.iov_base = &header, .iov_len = sizeof(header)},
                         {.iov_base = (void *)payload, .iov_len = payload_bytes}};
  struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
  if (nfd > 0) {
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfd);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * (size_t)nfd);
  }

  ssize_t sent = sendmsg(fd, &msg, MSG_NOSIGNAL);
  if (sent < 0)
    return -1;
  if ((size_t)sent != sizeof(header) + payload_bytes) {
    errno = EIO;
    return -1;
  }
  return 0;
}

int socket_v2_recv(int fd, uint16_t expected_type, void *payload, size_t payload_capacity, int fds[4], int *nfd) {
  if (fd < 0 || !payload || !fds || !nfd || payload_capacity > IDK_SOCKET_V2_MAX_PAYLOAD) {
    errno = EINVAL;
    return -1;
  }
  for (int i = 0; i < 4; i++)
    fds[i] = -1;
  *nfd = 0;

  size_t wire_capacity = sizeof(socket_v2_header_t) + payload_capacity;
  uint8_t *wire = malloc(wire_capacity ? wire_capacity : 1);
  if (!wire)
    return -1;
  char control[CMSG_SPACE(sizeof(int) * 16)] = {0};
  struct iovec iov = {.iov_base = wire, .iov_len = wire_capacity};
  struct msghdr msg = {.msg_control = control, .msg_controllen = sizeof(control), .msg_iov = &iov, .msg_iovlen = 1};
  ssize_t received = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
  if (received < 0) {
    free(wire);
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  }
  if (received == 0) {
    free(wire);
    errno = ECONNRESET;
    return -1;
  }
  int fd_rc = collect_fds(&msg, fds, nfd);
  bool bad = fd_rc != 0 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
  if ((size_t)received < sizeof(socket_v2_header_t))
    bad = true;

  socket_v2_header_t header = {0};
  if (!bad)
    memcpy(&header, wire, sizeof(header));
  if (!bad && (header.magic != IDK_SOCKET_V2_MAGIC || header.major != IDK_SOCKET_V2_MAJOR || header.minor != 0 ||
               header.type != expected_type || header.payload_bytes > payload_capacity ||
               sizeof(header) + (size_t)header.payload_bytes != (size_t)received))
    bad = true;
  if (bad) {
    close_fds(fds, nfd);
    free(wire);
    errno = EBADMSG;
    return -1;
  }

  memcpy(payload, wire + sizeof(header), header.payload_bytes);
  free(wire);
  return 1;
}

int socket_v2_peek_type(int fd, uint16_t *type) {
  socket_v2_header_t header;
  ssize_t n = recv(fd, &header, sizeof(header), MSG_PEEK | MSG_DONTWAIT);
  if (n < 0)
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  if (n == 0) {
    errno = ECONNRESET;
    return -1;
  }
  if ((size_t)n < sizeof(header) || header.magic != IDK_SOCKET_V2_MAGIC || header.major != IDK_SOCKET_V2_MAJOR ||
      header.minor != 0 || header.payload_bytes > IDK_SOCKET_V2_MAX_PAYLOAD) {
    errno = EBADMSG;
    return -1;
  }
  *type = header.type;
  return 1;
}
