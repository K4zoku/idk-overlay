#include "protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
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
static _Atomic uint64_t g_session_seed = 1;

uint64_t socket_v2_new_session_generation(void) {
  struct timespec now = {0};
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t serial = atomic_fetch_add_explicit(&g_session_seed, 1, memory_order_relaxed);
  uint64_t generation = ((uint64_t)getpid() << 32) ^ ((uint64_t)now.tv_sec << 17) ^ (uint64_t)now.tv_nsec ^ serial;
  return generation ? generation : serial + 1;
}

int socket_v2_send_hello(int fd, uint32_t features, const idk_socket_v2_identity_t *identity) {
  const idk_socket_features_t hello = {
      .features = features,
      .max_fds = 4,
      .reserved = 0,
  };
  return socket_v2_send(fd, IDK_SOCKET_V2_HELLO, &hello, sizeof(hello), identity, NULL, 0);
}

int socket_v2_server_handshake(int fd, uint32_t local_features, uint32_t *features, uint64_t *session_generation,
                               uint64_t *next_message_id) {
  idk_socket_features_t hello = {0};
  idk_socket_v2_identity_t hello_identity = {0};
  int fds[4];
  int nfd = 0;
  int rc = socket_v2_recv(fd, IDK_SOCKET_V2_HELLO, &hello, sizeof(hello), &hello_identity, fds, &nfd);
  for (int i = 0; i < nfd; i++)
    close(fds[i]);
  if (rc <= 0)
    return rc;
  if (nfd != 0 || hello_identity.session_generation == 0 || hello_identity.message_id == 0) {
    errno = EBADMSG;
    return -1;
  }
  if (hello.max_fds == 0 || !(hello.features & IDK_SOCKET_FEATURE_SEQPACKET) ||
      !(local_features & IDK_SOCKET_FEATURE_SEQPACKET)) {
    errno = EPROTONOSUPPORT;
    return -1;
  }
  idk_socket_features_t selected = {
      .features = hello.features & local_features,
      .max_fds = hello.max_fds < 4 ? hello.max_fds : 4,
      .reserved = 0,
  };
  idk_socket_v2_identity_t selected_identity = {
      .session_generation = hello_identity.session_generation,
      .message_id = 1,
  };
  if (socket_v2_send(fd, IDK_SOCKET_V2_FEATURES, &selected, sizeof(selected), &selected_identity, NULL, 0) < 0)
    return -1;
  if (features)
    *features = selected.features;
  if (session_generation)
    *session_generation = hello_identity.session_generation;
  if (next_message_id)
    *next_message_id = 2;
  return 1;
}

int socket_v2_client_handshake(int fd, uint64_t session_generation, uint32_t *features, uint64_t *next_message_id) {
  idk_socket_features_t selected = {0};
  idk_socket_v2_identity_t selected_identity = {0};
  int fds[4];
  int nfd = 0;
  int rc = socket_v2_recv(fd, IDK_SOCKET_V2_FEATURES, &selected, sizeof(selected), &selected_identity, fds, &nfd);
  for (int i = 0; i < nfd; i++)
    close(fds[i]);
  if (rc <= 0)
    return rc;
  if (nfd != 0 || session_generation == 0 || selected_identity.session_generation != session_generation ||
      selected_identity.message_id == 0 || selected.max_fds == 0 ||
      !(selected.features & IDK_SOCKET_FEATURE_SEQPACKET)) {
    errno = EPROTONOSUPPORT;
    return -1;
  }
  if (features)
    *features = selected.features;
  if (next_message_id)
    *next_message_id = 2;
  return 1;
}

int socket_v2_send(int fd, uint16_t type, const void *payload, size_t payload_bytes,
                   const idk_socket_v2_identity_t *identity, const int *fds, int nfd) {
  if (fd < 0 || !payload || !identity || identity->session_generation == 0 || identity->message_id == 0 ||
      payload_bytes > IDK_SOCKET_V2_MAX_PAYLOAD || nfd < 0 || nfd > 4 || (nfd > 0 && !fds)) {
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
      .session_generation = identity->session_generation,
      .message_id = identity->message_id,
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
int socket_v2_recv(int fd, uint16_t expected_type, void *payload, size_t payload_capacity,
                   idk_socket_v2_identity_t *identity, int fds[4], int *nfd) {
  if (fd < 0 || !payload || !identity || !fds || !nfd || payload_capacity > IDK_SOCKET_V2_MAX_PAYLOAD) {
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
               header.session_generation == 0 || header.message_id == 0 || header.type != expected_type ||
               header.payload_bytes > payload_capacity || sizeof(header) + (size_t)header.payload_bytes != (size_t)received))
    bad = true;
  if (bad) {
    close_fds(fds, nfd);
    free(wire);
    errno = EBADMSG;
    return -1;
  }
  *identity = (idk_socket_v2_identity_t){
      .session_generation = header.session_generation,
      .message_id = header.message_id,
  };
  memcpy(payload, wire + sizeof(header), header.payload_bytes);
  free(wire);
  return 1;
}

static int socket_v2_recv_without_fds(int fd, uint16_t type, void *payload, size_t payload_bytes,
                                      idk_socket_v2_identity_t *identity) {
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  int rc = socket_v2_recv(fd, type, payload, payload_bytes, identity, fds, &nfd);
  if (rc == 1 && nfd != 0) {
    close_fds(fds, &nfd);
    errno = EBADMSG;
    return -1;
  }
  return rc;
}

int socket_v2_send_ring_create(int fd, const idk_shm_ring_create_t *record,
                               const idk_socket_v2_identity_t *identity, int ring_fd) {
  if (!record || ring_fd < 0)
    return (errno = EINVAL), -1;
  return socket_v2_send(fd, IDK_SOCKET_V2_RING_CREATE, record, sizeof(*record), identity, &ring_fd, 1);
}

int socket_v2_recv_ring_create(int fd, idk_shm_ring_create_t *record, idk_socket_v2_identity_t *identity, int *ring_fd) {
  if (!record || !ring_fd)
    return (errno = EINVAL), -1;
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  int rc = socket_v2_recv(fd, IDK_SOCKET_V2_RING_CREATE, record, sizeof(*record), identity, fds, &nfd);
  if (rc != 1)
    return rc;
  if (nfd != 1 || record->magic != IDK_SHM_RING_MAGIC || record->version != IDK_SHM_RING_VERSION ||
      record->slot_count == 0 || record->slot_count > IDK_SHM_RING_MAX_SLOTS || record->slot_capacity == 0 ||
      record->ring_bytes != (uint64_t)record->slot_count * record->slot_capacity ||
      record->session_generation != identity->session_generation) {
    close_fds(fds, &nfd);
    errno = EBADMSG;
    return -1;
  }
  *ring_fd = fds[0];
  return 1;
}

int socket_v2_send_ring_ready(int fd, const idk_shm_ring_ready_t *record, const idk_socket_v2_identity_t *identity) {
  return socket_v2_send(fd, IDK_SOCKET_V2_RING_READY, record, sizeof(*record), identity, NULL, 0);
}

int socket_v2_recv_ring_ready(int fd, idk_shm_ring_ready_t *record, idk_socket_v2_identity_t *identity) {
  int rc = socket_v2_recv_without_fds(fd, IDK_SOCKET_V2_RING_READY, record, sizeof(*record), identity);
  if (rc == 1 && (record->session_generation != identity->session_generation || record->slot_count == 0 ||
                  record->slot_count > IDK_SHM_RING_MAX_SLOTS)) {
    errno = EBADMSG;
    return -1;
  }
  return rc;
}

int socket_v2_send_frame_slot(int fd, const idk_shm_ring_frame_slot_t *record,
                              const idk_socket_v2_identity_t *identity) {
  return socket_v2_send(fd, IDK_SOCKET_V2_FRAME_SLOT, record, sizeof(*record), identity, NULL, 0);
}

int socket_v2_recv_frame_slot(int fd, idk_shm_ring_frame_slot_t *record, idk_socket_v2_identity_t *identity) {
  int rc = socket_v2_recv_without_fds(fd, IDK_SOCKET_V2_FRAME_SLOT, record, sizeof(*record), identity);
  if (rc != 1)
    return rc;
  const bool full = (record->flags & IDK_SHM_RING_FRAME_FULL) != 0;
  const bool roi = (record->flags & IDK_SHM_RING_FRAME_ROI) != 0;
  if (record->session_generation != identity->session_generation || record->frame_id == 0 ||
      record->slot_index >= IDK_SHM_RING_MAX_SLOTS || record->payload_bytes == 0 ||
      (record->flags & ~(IDK_SHM_RING_FRAME_FULL | IDK_SHM_RING_FRAME_ROI)) != 0 || (!full && !roi) ||
      (full && roi) || (roi && (record->roi_width == 0 || record->roi_height == 0))) {
    errno = EBADMSG;
    return -1;
  }
  return 1;
}

int socket_v2_send_slot_release(int fd, const idk_shm_ring_slot_release_t *record,
                                const idk_socket_v2_identity_t *identity) {
  return socket_v2_send(fd, IDK_SOCKET_V2_SLOT_RELEASE, record, sizeof(*record), identity, NULL, 0);
}

int socket_v2_recv_slot_release(int fd, idk_shm_ring_slot_release_t *record, idk_socket_v2_identity_t *identity) {
  int rc = socket_v2_recv_without_fds(fd, IDK_SOCKET_V2_SLOT_RELEASE, record, sizeof(*record), identity);
  if (rc == 1 &&
      (record->session_generation != identity->session_generation || record->frame_id == 0 ||
       record->slot_index >= IDK_SHM_RING_MAX_SLOTS)) {
    errno = EBADMSG;
    return -1;
  }
  return rc;
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
      header.minor != 0 || header.session_generation == 0 || header.message_id == 0 ||
      header.payload_bytes > IDK_SOCKET_V2_MAX_PAYLOAD) {
    errno = EBADMSG;
    return -1;
  }
  *type = header.type;
  return 1;
}
