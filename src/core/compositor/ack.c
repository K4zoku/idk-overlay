#include "core/compositor.h"
#include "core/log.h"

/* ===== Shared compositor API ===== */

void idk_compositor_send_ack(uint8_t code) {
  if (!g_comp.tp.ready)
    return;
  idk_ack_msg_t msg;
  idk_comp_build_ack(&msg, code, g_comp.game_w, g_comp.game_h, &g_comp.size_pending, &g_comp.last_resize_ts,
                     IDK_COMP_RESIZE_DEBOUNCE_MS, "comp");
  idk_tp_send_ack(&g_comp.tp, &msg);
}

void idk_compositor_send_request(void) {
  if (!g_comp.tp.ready)
    return;
  idk_request_msg_t req = {0};
  req.type = IDK_REQUEST_NEXT_FRAME;
  idk_tp_send_request(&g_comp.tp, &req);
}

void idk_compositor_notify_resize(int w, int h) {
  idk_comp_notify_resize(&g_comp.game_w, &g_comp.game_h, &g_comp.size_pending, &g_comp.last_resize_ts, w, h, "comp");
}
