#include <time.h>
#include <unistd.h>

#include "core/compositor.h"
#include "core/log.h"

/* ===== Shared compositor API ===== */

/* Ensure transport is inited and connected. Returns -1 on init failure,
 * 0 if not ready, 1 if ready. */
static int recv_accept(void) {
  if (!g_comp.inited && idk_compositor_init() != 0)
    return -1;
  idk_tp_accept(&g_comp.tp);
  return g_comp.tp.ready ? 1 : 0;
}

/* Drop all queued frames while hidden. */
static void drain_hidden(void) {
  while (1) {
    int rc = idk_tp_drop_frame(&g_comp.tp);
    if (rc <= 0) {
      if (rc < 0) {
        g_comp.dmabuf_cache_id = 0;
        idk_tp_disconnect_client(&g_comp.tp);
      }
      break;
    }
  }
  g_comp.was_hidden = true;
}

/* Recv loop: keep the first frame of the batch, close surplus dmabuf fds. */
static int keep_last_frame(void) {
  int processed = 0;
  while (1) {
    idk_frame_header_t hdr;
    int fds[4], nfd = 0;
    int rc = idk_tp_recv(&g_comp.tp, &hdr, fds, &nfd);
    if (rc <= 0) {
      if (rc < 0) {
        idk_tp_disconnect_client(&g_comp.tp);
        g_comp.dmabuf_cache_id = 0;
      }
      break;
    }

    int dmabuf_fd = (nfd > 0) ? fds[0] : -1;

    if (processed > 0) {
      if (dmabuf_fd >= 0)
        close(dmabuf_fd);
      continue;
    }

    if (g_comp.dmabuf_fd[0] >= 0) {
      close(g_comp.dmabuf_fd[0]);
      g_comp.dmabuf_fd[0] = -1;
    }

    g_comp.hdr = hdr;
    g_comp.dmabuf_fd[0] = dmabuf_fd;
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

  idk_request_msg_t req = {0};
  req.type = IDK_REQUEST_NEXT_FRAME;
  idk_tp_send_request(&g_comp.tp, &req);

  return processed ? 1 : 0;
}
