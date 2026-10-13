#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/compositor.h"
#include "core/log.h"
#include "../transport/socket/protocol.h"
#include "../linux/fd.h"
#include "public/idk_shm_delta.h"

/* ===== Shared compositor API ===== */

void idk_compositor_close_frame_fds(int fds[4], int *nfd) {
  if (!fds || !nfd)
    return;
  for (int i = 0; i < 4; i++) {
    if (fds[i] >= 0) {
      close(fds[i]);
      fds[i] = -1;
    }
  }
  *nfd = 0;
}

static void disconnect_client(void) {
  idk_tp_disconnect_client(&g_comp.tp);
  if (g_comp.ring_map) {
    munmap(g_comp.ring_map, g_comp.ring_bytes);
    g_comp.ring_map = NULL;
    g_comp.ring_bytes = 0;
    g_comp.ring_generation = 0;
    g_comp.ring_slot_count = 0;
    g_comp.ring_slot_capacity = 0;
  }
  g_comp.ring_frame_active = false;
  g_comp.ring_request_deferred = false;
  g_comp.ring_frame_id = 0;
  g_comp.ring_frame_slot = 0;
  g_comp.ring_frame_offset = 0;
  g_comp.ring_frame_bytes = 0;
  idk_compositor_close_frame_fds(g_comp.dmabuf_fd, &g_comp.nfd);
  g_comp.has_frame = false;
  g_comp.dmabuf_cache_id = 0;
}

/* Ensure transport is inited and connected. Returns -1 on init failure,
 * 0 if not ready, 1 if ready. */
static int recv_accept(void) {
  if (!g_comp.inited && idk_compositor_init() != 0)
    return -1;
  idk_tp_accept(&g_comp.tp);
  return g_comp.tp.ready ? 1 : 0;
}

static int receive_ring_create(void);
static int receive_ring_frame(bool keep);

/* Drop all queued frames while hidden. */
static void drain_hidden(void) {
  while (1) {
    uint16_t type = 0;
    int peek = idk_tp_peek_type(&g_comp.tp, &type);
    if (peek == 1 && type == IDK_SOCKET_V2_RING_CREATE) {
      if (receive_ring_create() < 0) {
        disconnect_client();
        break;
      }
      continue;
    }
    if (peek == 1 && type == IDK_SOCKET_V2_FRAME_SLOT) {
      if (receive_ring_frame(false) < 0) {
        disconnect_client();
        break;
      }
      idk_ack_msg_t ack = {.ack = IDK_ACK_DISCARDED};
      idk_tp_send_ack(&g_comp.tp, &ack);
      continue;
    }
    if (peek == 1 && type != IDK_SOCKET_V2_FRAME) {
      disconnect_client();
      break;
    }
    int rc = idk_tp_drop_frame(&g_comp.tp);
    if (rc <= 0) {
      if (rc < 0)
        disconnect_client();
      break;
    }
    idk_ack_msg_t ack = {.ack = IDK_ACK_DISCARDED};
    idk_tp_send_ack(&g_comp.tp, &ack);
  }
  g_comp.was_hidden = true;
}

static int receive_ring_create(void) {
  idk_shm_ring_create_t record = {0};
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  int rc = idk_tp_recv_control(&g_comp.tp, IDK_SOCKET_V2_RING_CREATE, &record, sizeof(record), fds, &nfd);
  if (rc <= 0)
    return rc;
  if (nfd != 1 || record.magic != IDK_SHM_RING_MAGIC || record.version != IDK_SHM_RING_VERSION ||
      record.slot_count == 0 || record.slot_count > IDK_SHM_RING_MAX_SLOTS || record.slot_capacity == 0 ||
      record.ring_bytes != (uint64_t)record.slot_count * record.slot_capacity ||
      record.ring_bytes > SIZE_MAX || record.width == 0 || record.height == 0 ||
      record.width > 65536 || record.height > 65536 || record.stride < record.width * 4 ||
      record.session_generation != idk_tp_session_generation(&g_comp.tp)) {
    idk_compositor_close_frame_fds(fds, &nfd);
    return -1;
  }
  struct stat st = {0};
  if (fstat(fds[0], &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size < record.ring_bytes) {
    idk_compositor_close_frame_fds(fds, &nfd);
    return -1;
  }
  int ring_fd = fds[0];
  fds[0] = -1;
  idk_compositor_close_frame_fds(fds, &nfd);
  void *map = mmap(NULL, (size_t)record.ring_bytes, PROT_READ, MAP_SHARED, ring_fd, 0);
  close(ring_fd);
  if (map == MAP_FAILED)
    return -1;
  if (g_comp.ring_map)
    munmap(g_comp.ring_map, g_comp.ring_bytes);
  g_comp.ring_map = map;
  g_comp.ring_bytes = (size_t)record.ring_bytes;
  g_comp.ring_generation = record.session_generation;
  g_comp.ring_slot_count = record.slot_count;
  g_comp.ring_slot_capacity = record.slot_capacity;
  idk_shm_ring_ready_t ready = {
      .session_generation = record.session_generation,
      .slot_count = record.slot_count,
  };
  if (idk_tp_send_control(&g_comp.tp, IDK_SOCKET_V2_RING_READY, &ready, sizeof(ready), NULL, 0) < 0)
    return -1;
  return 1;
}
static int receive_ring_frame(bool keep) {
  idk_shm_ring_frame_slot_t record = {0};
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  int rc = idk_tp_recv_control(&g_comp.tp, IDK_SOCKET_V2_FRAME_SLOT, &record, sizeof(record), fds, &nfd);
  if (rc <= 0)
    return rc;
  bool addressable = nfd == 0 && g_comp.ring_map && record.session_generation == g_comp.ring_generation &&
                     record.frame_id != 0 && record.slot_index < g_comp.ring_slot_count &&
                     (uint64_t)record.offset == (uint64_t)record.slot_index * g_comp.ring_slot_capacity;
  if (!addressable) {
    idk_compositor_close_frame_fds(fds, &nfd);
    return -1;
  }
  if (record.payload_bytes == 0 || record.payload_bytes > g_comp.ring_slot_capacity ||
      (uint64_t)record.offset > g_comp.ring_bytes ||
      (uint64_t)record.payload_bytes > g_comp.ring_bytes - record.offset ||
      (record.flags & ~(IDK_SHM_RING_FRAME_FULL | IDK_SHM_RING_FRAME_ROI)) != 0 ||
      ((record.flags & IDK_SHM_RING_FRAME_FULL) == 0) == ((record.flags & IDK_SHM_RING_FRAME_ROI) == 0)) {
    idk_shm_ring_slot_release_t release = {
        .session_generation = record.session_generation,
        .frame_id = record.frame_id,
        .slot_index = record.slot_index,
        .status = 1,
    };
    (void)idk_tp_send_control(&g_comp.tp, IDK_SOCKET_V2_SLOT_RELEASE, &release, sizeof(release), NULL, 0);
    return -1;
  }
  const uint8_t *source = (const uint8_t *)g_comp.ring_map + record.offset;
  size_t expected_full = (size_t)record.width * record.height * 4u;
  bool full = (record.flags & IDK_SHM_RING_FRAME_FULL) != 0;
  bool valid_payload = record.width != 0 && record.height != 0 && record.width <= 65536 && record.height <= 65536 &&
                       (full ? record.payload_bytes == expected_full
                             : idk_shm_delta_validate((const idk_shm_delta_t *)source, record.width, record.height,
                                                      record.payload_bytes));
  if (!valid_payload) {
    idk_shm_ring_slot_release_t release = {
        .session_generation = record.session_generation,
        .frame_id = record.frame_id,
        .slot_index = record.slot_index,
        .status = 1,
    };
    (void)idk_tp_send_control(&g_comp.tp, IDK_SOCKET_V2_SLOT_RELEASE, &release, sizeof(release), NULL, 0);
    return -1;
  }
  idk_shm_ring_slot_release_t release = {
      .session_generation = record.session_generation,
      .frame_id = record.frame_id,
      .slot_index = record.slot_index,
      .status = 0,
  };
  if (!keep) {
    if (idk_tp_send_control(&g_comp.tp, IDK_SOCKET_V2_SLOT_RELEASE, &release, sizeof(release), NULL, 0) < 0)
      return -1;
    return 0;
  }
  if (g_comp.ring_frame_active) {
    release.status = 1;
    if (idk_tp_send_control(&g_comp.tp, IDK_SOCKET_V2_SLOT_RELEASE, &release, sizeof(release), NULL, 0) < 0)
      return -1;
    return 0;
  }
  g_comp.hdr = (idk_frame_header_t){
      .width = record.width,
      .height = record.height,
      .flags = IDK_FRAME_FLAG_VISIBLE | (full ? 0 : IDK_FRAME_FLAG_SHM_RECT),
      .nfd = 0,
  };
  g_comp.ring_frame_active = true;
  g_comp.ring_frame_id = record.frame_id;
  g_comp.ring_frame_slot = record.slot_index;
  g_comp.ring_frame_offset = record.offset;
  g_comp.ring_frame_bytes = record.payload_bytes;
  g_comp.frame_w = record.width;
  g_comp.frame_h = record.height;
  g_comp.has_frame = true;
  clock_gettime(CLOCK_MONOTONIC, &g_comp.last_frame_ts);
  return 1;
}

int idk_compositor_release_ring_slot(uint32_t status) {
  if (!g_comp.ring_frame_active)
    return 0;
  idk_shm_ring_slot_release_t release = {
      .session_generation = g_comp.ring_generation,
      .frame_id = g_comp.ring_frame_id,
      .slot_index = g_comp.ring_frame_slot,
      .status = status,
  };
  int rc = idk_tp_send_control(&g_comp.tp, IDK_SOCKET_V2_SLOT_RELEASE, &release, sizeof(release), NULL, 0);
  g_comp.ring_frame_active = false;
  g_comp.ring_frame_id = 0;
  g_comp.ring_frame_slot = 0;
  g_comp.ring_frame_offset = 0;
  g_comp.ring_frame_bytes = 0;
  if (g_comp.ring_request_deferred) {
    g_comp.ring_request_deferred = false;
    idk_compositor_send_request();
  }
  return rc < 0 ? -1 : 0;
}

/* Recv loop: keep the first frame of the batch and close all surplus FDs. */
static int keep_last_frame(void) {
  int processed = 0;
  while (1) {
    uint16_t type = 0;
    int peek = idk_tp_peek_type(&g_comp.tp, &type);
    if (peek == 1 && type == IDK_SOCKET_V2_RING_CREATE) {
      if (receive_ring_create() < 0) {
        disconnect_client();
        break;
      }
      continue;
    }
    if (peek == 1 && type == IDK_SOCKET_V2_FRAME_SLOT) {
      int ring_rc = receive_ring_frame(processed == 0);
      if (ring_rc < 0) {
        disconnect_client();
        break;
      }
      if (ring_rc > 0) {
        processed = 1;
        g_comp.ring_request_deferred = true;
      }
      continue;
    }
    if (peek == 1 && type != IDK_SOCKET_V2_FRAME) {
      disconnect_client();
      break;
    }
    idk_frame_header_t hdr;
    int fds[4] = {-1, -1, -1, -1};
    int nfd = 0;
    int rc = idk_tp_recv(&g_comp.tp, &hdr, fds, &nfd);
    if (rc <= 0) {
      if (rc < 0)
        disconnect_client();
      break;
    }
    if (nfd < 1 || nfd > 4 || hdr.width == 0 || hdr.height == 0) {
      idk_compositor_close_frame_fds(fds, &nfd);
      continue;
    }
    if (processed > 0) {
      idk_compositor_close_frame_fds(fds, &nfd);
      continue;
    }
    idk_compositor_close_frame_fds(g_comp.dmabuf_fd, &g_comp.nfd);
    g_comp.hdr = hdr;
    for (int i = 0; i < nfd; i++)
      g_comp.dmabuf_fd[i] = fds[i];
    g_comp.nfd = nfd;
    g_comp.frame_w = hdr.width;
    g_comp.frame_h = hdr.height;
    g_comp.has_frame = true;
    processed = 1;
    clock_gettime(CLOCK_MONOTONIC, &g_comp.last_frame_ts);
  }
  return processed;
}

int idk_compositor_recv_frame(bool visible) {
  int rc = recv_accept();
  if (rc <= 0)
    return rc;

  if (!visible) {
    drain_hidden();
    return 0;
  }

  if (g_comp.was_hidden) {
    g_comp.was_hidden = false;
    idk_request_msg_t wake = {0};
    wake.type = IDK_REQUEST_NEXT_FRAME;
    idk_tp_send_request(&g_comp.tp, &wake);
  }

  int processed = keep_last_frame();

  if (!g_comp.ring_request_deferred) {
    idk_request_msg_t req = {0};
    req.type = IDK_REQUEST_NEXT_FRAME;
    idk_tp_send_request(&g_comp.tp, &req);
  }

  return processed ? 1 : 0;
}
