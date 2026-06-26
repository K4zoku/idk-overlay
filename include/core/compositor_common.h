/*
 * compositor_common.h — shared code between GL and Vulkan compositors.
 *
 * Contains the frame protocol (idk_frame_header_t from idk_ipc.h),
 * socket layer (listen/accept/recvmsg), ACK logic with resize debounce,
 * and cross-GPU dmabuf vendor detection helpers.
 *
 * Both compositor.c (GL) and compositor_vk.c (VK) use these to avoid
 * duplicating the ~250 lines of socket + frame-receive + ACK code that
 * is identical between the two paths.
 */

#ifndef IDK_COMPOSITOR_COMMON_H
#define IDK_COMPOSITOR_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdbool.h>

#include "public/idk_ipc.h"  /* idk_frame_header_t, IDK_FRAME_FLAG_* */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Frame protocol helpers ───────────────────────────────────────────── */
/* idk_frame_header_t, IDK_FRAME_FLAG_VISIBLE, IDK_FRAME_FLAG_DMABUF are
 * defined in include/public/idk_ipc.h. */

/* Returns true if the frame is a DMABUF frame (vs SHM). */
static inline bool idk_frame_is_dmabuf(const idk_frame_header_t *hdr) {
    return (hdr->flags & IDK_FRAME_FLAG_DMABUF) != 0;
}

/* Returns true if the frame is visible. */
static inline bool idk_frame_is_visible(const idk_frame_header_t *hdr) {
    return (hdr->flags & IDK_FRAME_FLAG_VISIBLE) != 0;
}

/* ── ACK message (1B ack + 4B w + 4B h + 3B pad) ─────────────────────── */

struct idk_ack_msg {
    uint8_t  ack;     /* 0 = accepted, 1 = rejected (DMABUF not supported) */
    int32_t  w;       /* game width (0 = no resize) */
    int32_t  h;       /* game height (0 = no resize) */
    uint8_t  _pad[3]; /* reserved */
};

/* ── Resize debounce ──────────────────────────────────────────────────── */

#define IDK_COMP_RESIZE_DEBOUNCE_MS 50

/* Update game size + timestamp. Returns true if size changed. */
bool idk_comp_notify_resize(int *game_w, int *game_h, bool *size_pending,
                            struct timespec *last_resize_ts,
                            int w, int h, const char *log_tag);

/* Check if resize has been stable for >debounce_ms. */
bool idk_comp_resize_stable(const struct timespec *last_resize_ts, int debounce_ms);

/* ── Socket layer ─────────────────────────────────────────────────────── */

/* Initialize listening socket. Path from IDK_SOCKET env or /tmp/idk-overlay-<pid>.
 * On success: out_listen_fd set. Returns 0 on success, -1 on failure. */
int idk_comp_sock_init(int *out_listen_fd, char *path, size_t path_sz,
                       const char *log_tag);

/* Non-blocking accept. Returns 1 if connected, 0 if no pending, -1 on error. */
int idk_comp_sock_accept(int listen_fd, int *out_client_fd, const char *log_tag);

/* Receive one frame from the client socket (non-blocking).
 * On success: fills out_hdr, sets *out_fd (caller owns).
 * Returns 1 if frame received, 0 if no data, -1 on error/disconnect. */
int idk_comp_recv_frame(int client_fd, idk_frame_header_t *out_hdr,
                        int *out_fd, const char *log_tag);

/* Send ACK to client. If size_pending && resize stable, embed w/h. */
void idk_comp_send_ack(int client_fd, uint8_t ack,
                       int game_w, int game_h,
                       bool *size_pending,
                       const struct timespec *last_resize_ts,
                       int debounce_ms, const char *log_tag);

/* ── Cross-GPU dmabuf vendor detection ────────────────────────────────── */

/* DRM modifier vendor bits (56-63). */
#define IDK_DRM_MOD_VENDOR(mod) (((mod) >> 56) & 0xFF)
#define IDK_DRM_FORMAT_MOD_INVALID 0x00FFFFFFFFFFFFFFULL

/* Map VkPhysicalDeviceProperties.vendorID → DRM vendor ID.
 *   0x10DE (NVIDIA) → 0x03, 0x8086 (Intel) → 0x01, 0x1002 (AMD) → 0x02.
 * Returns 0 for unknown vendors. */
uint32_t idk_vk_vendor_to_drm(uint32_t vk_vendor);

#ifdef __cplusplus
}
#endif

#endif /* IDK_COMPOSITOR_COMMON_H */
