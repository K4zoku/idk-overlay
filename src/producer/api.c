/* Frame sender library (client side of compositor). */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include "../core/linux/fd.h"
#include "core/log.h"
#include "core/diagnostics.h"
#include "core/transport.h"
#include "public/idk_ipc.h"
#include "public/idk_producer.h"
#include "public/idk_shm_delta.h"

/* Internal state */

static idk_transport_t g_tp = {.backend = IDK_TP_SOCKET, ._server_fd = -1, ._client_fd = -1};

/* SHM helper - copy pixels into an anonymous, size-sealed snapshot. */
static int copy_to_shm(const void *src, size_t size) {
  int fd = idk_memfd_copy(src, size, "idk-frame");
  if (fd < 0)
    IDK_ERR("producer", "frame snapshot allocation failed: %s\n", strerror(errno));
  return fd;
}

int idk_producer_init(const char *sockpath) {
  if (!sockpath) {
    errno = EINVAL;
    return -1;
  }
  size_t len = strlen(sockpath);
  if (len >= IDK_TP_PATH_MAX) {
    errno = ERANGE;
    return -1;
  }
  return idk_tp_init(&g_tp, IDK_TP_PRODUCER, sockpath);
}

bool idk_producer_is_connected(void) { return g_tp.ready; }

void idk_producer_shutdown(void) { idk_tp_destroy(&g_tp); }

/* Internal: sendmsg with frame header + N fds. */
static int send_frame_msg(const idk_frame_header_t *hdr, const int *fds, int nfd) {
  if (!g_tp.ready || !hdr || !fds || nfd < 1 || nfd > 4) {
    errno = EINVAL;
    return -1;
  }

  int rc = idk_tp_send(&g_tp, hdr, fds, nfd);
  if (rc == 0)
    idk_diag_inc((hdr->flags & IDK_FRAME_FLAG_SHM_RECT) ? IDK_DIAG_FRAME_SENT_ROI : IDK_DIAG_FRAME_SENT_FULL);
  return rc;
}

int idk_producer_send_frame(int data_fd, const idk_frame_header_t *hdr) {
  if (!hdr) {
    errno = EINVAL;
    return -1;
  }
  int rc = send_frame_msg(hdr, &data_fd, 1);
  if (rc == 0) {
    IDK_LOG("producer", "Frame sent: %dx%d stride=%u flags=0x%02x fd=%d\n", hdr->width, hdr->height, hdr->stride,
            hdr->flags, data_fd);
  }
  return rc;
}

int idk_producer_send_pixels(const void *pixels, const idk_frame_header_t *hdr) {
  if (!pixels || !hdr) {
    errno = EINVAL;
    return -1;
  }

  idk_frame_header_t f = *hdr;
  f.flags = IDK_FRAME_FLAG_VISIBLE;
  f.stride = 0;

  if (f.width == 0 || f.height == 0 || f.width > 65536 || f.height > 65536) {
    IDK_ERR("producer", "idk_producer_send_pixels: invalid dimensions %ux%u\n", (unsigned)f.width, (unsigned)f.height);
    errno = EINVAL;
    return -1;
  }

  size_t pixel_size = (size_t)f.width * (size_t)f.height * 4;
  int shm_fd = copy_to_shm(pixels, pixel_size);
  if (shm_fd < 0) {
    IDK_ERR("producer", "copy_to_shm failed\n");
    return -1;
  }

  int rc = idk_producer_send_frame(shm_fd, &f);
  close(shm_fd);

  return rc;
}

int idk_producer_send_dma_buf(const int *dma_buf_fds, const idk_frame_header_t *hdr) {
  if (!hdr || !dma_buf_fds || hdr->nfd == 0 || hdr->nfd > 4) {
    errno = EINVAL;
    return -1;
  }

  idk_frame_header_t f = *hdr;
  f.flags = (f.flags & ~IDK_FRAME_FLAG_DMABUF) | IDK_FRAME_FLAG_DMABUF;

  int rc = send_frame_msg(&f, dma_buf_fds, f.nfd);
  if (rc == 0) {
  }

  return rc;
}

int idk_producer_send_control(uint16_t type, const void *payload, size_t payload_bytes, const int *fds, int nfd) {
  if (!g_tp.ready) {
    errno = ENOTCONN;
    return -1;
  }
  return idk_tp_send_control(&g_tp, type, payload, payload_bytes, fds, nfd);
}
int idk_producer_recv_control(uint16_t type, void *payload, size_t payload_capacity) {
  if (!g_tp.ready) {
    errno = ENOTCONN;
    return -1;
  }
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  int rc = idk_tp_recv_control(&g_tp, type, payload, payload_capacity, fds, &nfd);
  for (int i = 0; i < nfd; i++)
    close(fds[i]);
  return rc;
}

int idk_producer_peek_type(uint16_t *type) {
  if (!g_tp.ready || !type) {
    errno = g_tp.ready ? EINVAL : ENOTCONN;
    return -1;
  }
  return idk_tp_peek_type(&g_tp, type);
}


uint64_t idk_producer_session_generation(void) {
  return idk_tp_session_generation(&g_tp);
}

int idk_producer_wait_ack(idk_ack_msg_t *ack, int timeout_ms) {
  if (!g_tp.ready) {
    errno = ENOTCONN;
    return -1;
  }
  return idk_tp_wait_ack(&g_tp, ack, timeout_ms);
}

int idk_producer_recv_request(idk_request_msg_t *req, int timeout_ms) {
  if (!g_tp.ready) {
    errno = ENOTCONN;
    return -1;
  }
  return idk_tp_recv_request(&g_tp, req, timeout_ms);
}

/* Socket fd carrying ACK/REQUEST data, for event-loop integration.
 * Returns -1 when not connected or the backend is not fd-pollable. */
int idk_producer_recv_request_preserving_ack(idk_request_msg_t *req, int timeout_ms) {
  if (!g_tp.ready) {
    errno = ENOTCONN;
    return -1;
  }
  return idk_tp_recv_request_preserving_ack(&g_tp, req, timeout_ms);
}

int idk_producer_poll_fd(void) {
  if (!g_tp.ready || g_tp.backend != IDK_TP_SOCKET)
    return -1;
  return g_tp._client_fd;
}
