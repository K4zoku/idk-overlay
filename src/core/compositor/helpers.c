#include <string.h>
#include <time.h>

#include "core/compositor.h"
#include "core/log.h"

/* ===== Resize debounce ===== */

bool idk_comp_notify_resize(int *game_w, int *game_h, bool *size_pending, struct timespec *last_resize_ts, int w, int h,
                            const char *log_tag) {
  if (w < 1 || h < 1)
    return false;
  if (w != *game_w || h != *game_h) {
    IDK_LOG(log_tag, "resize: %dx%d -> %dx%d\n", *game_w, *game_h, w, h);
    *game_w = w;
    *game_h = h;
    *size_pending = true;
    clock_gettime(CLOCK_MONOTONIC, last_resize_ts);
    return true;
  }
  return false;
}

bool idk_comp_resize_stable(const struct timespec *last_resize_ts, int debounce_ms) {
  if (last_resize_ts->tv_sec == 0 && last_resize_ts->tv_nsec == 0)
    return true;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long delta_ms = (now.tv_sec - last_resize_ts->tv_sec) * 1000L + (now.tv_nsec - last_resize_ts->tv_nsec) / 1000000L;
  return delta_ms >= debounce_ms;
}

/* ===== ACK builder ===== */

void idk_comp_build_ack(idk_ack_msg_t *msg, uint8_t ack, int game_w, int game_h, bool *size_pending,
                        const struct timespec *last_resize_ts, int debounce_ms, const char *log_tag) {
  memset(msg, 0, sizeof(*msg));
  msg->ack = ack;
  if (*size_pending && idk_comp_resize_stable(last_resize_ts, debounce_ms)) {
    msg->w = game_w;
    msg->h = game_h;
    *size_pending = false;
    IDK_LOG(log_tag, "ACK with size %dx%d (ack=%d)\n", game_w, game_h, ack);
  } else if (*size_pending) {
    IDK_LOG(log_tag, "ACK without size (debouncing, ack=%d)\n", ack);
  }
}

/* ===== Cross-GPU dmabuf vendor detection ===== */

uint32_t idk_vk_vendor_to_drm(uint32_t vk_vendor) {
  switch (vk_vendor) {
  case 0x10DE:
    return 0x03;
  case 0x8086:
    return 0x01;
  case 0x1002:
    return 0x02;
  default:
    return 0;
  }
}
