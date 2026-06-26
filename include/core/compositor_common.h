/*
 * compositor_common.h — shared code between GL and Vulkan compositors.
 *
 * Contains the frame protocol (struct frame_hdr, frame type constants),
 * socket layer (listen/accept/recvmsg), ACK logic with resize debounce,
 * and cross-GPU dmabuf vendor detection helpers.
 *
 * Both compositor_gl.c and compositor_vk.c use these to avoid duplicating
 * the ~250 lines of socket + frame-receive + ACK code that is identical
 * between the two paths.
 */

#ifndef IDK_COMPOSITOR_COMMON_H
#define IDK_COMPOSITOR_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Frame protocol (40-byte header sent over the socket) ─────────────── */

#define IDK_FRAME_TYPE_DMABUF  0
#define IDK_FRAME_TYPE_SHM     1

#define IDK_SHM_FORMAT_STRAIGHT       0
#define IDK_SHM_FORMAT_PREMULTIPLIED  1

struct idk_frame_hdr {
    uint32_t width;
    uint32_t height;
    uint32_t stride;       /* DMABUF: bytes per row; SHM: unused */
    uint32_t format;       /* DMABUF: DRM fourcc; SHM: premul flag */
    uint32_t num_planes;   /* DMABUF: plane count; SHM: buf_idx */
    uint32_t reserved;     /* SHM: pixel byte size */
    uint32_t vis_type;     /* low byte: visibility; high byte: frame type */
    uint32_t checksum;
    uint64_t modifier;     /* DMABUF: tiling modifier; SHM: unused */
};

/* Extract frame type from vis_type field. */
static inline uint8_t idk_frame_type(const struct idk_frame_hdr *hdr) {
    return (hdr->vis_type >> 8) & 0xFF;
}

/* ── ACK message (9 bytes: 1 byte ack + 4 bytes w + 4 bytes h) ────────── */

struct idk_ack_msg {
    uint8_t  ack;   /* 0 = accepted, 1 = rejected (DMABUF not supported) */
    int32_t  w;     /* game width (0 = no resize) */
    int32_t  h;     /* game height (0 = no resize) */
};

/* ── Resize debounce ──────────────────────────────────────────────────── */

#define IDK_COMP_RESIZE_DEBOUNCE_MS 50

/* Update game size + timestamp. Returns true if size changed. */
bool idk_comp_notify_resize(int *game_w, int *game_h, bool *size_pending,
                            struct timespec *last_resize_ts,
                            int w, int h, const char *log_tag);

/* Check if resize has been stable for >debounce_ms. Returns true if ready
 * to embed size in ACK. */
bool idk_comp_resize_stable(const struct timespec *last_resize_ts, int debounce_ms);

/* ── Socket layer ─────────────────────────────────────────────────────── */

/* Initialize listening socket. Path from IDK_SOCKET env or /tmp/idk-overlay-<pid>.
 * On success: out_listen_fd set, out_client_fd = -1 (accept later).
 * Returns 0 on success, -1 on failure. */
int idk_comp_sock_init(int *out_listen_fd, char *path, size_t path_sz,
                       const char *log_tag);

/* Non-blocking accept. If a client connects, sets *out_client_fd (non-blocking)
 * and returns 1. Returns 0 if no pending connection, -1 on error. */
int idk_comp_sock_accept(int listen_fd, int *out_client_fd, const char *log_tag);

/* Receive one frame from the client socket (non-blocking).
 * On success: fills out_hdr, sets *out_fd to the dmabuf/shm fd (caller owns).
 * Returns 1 if a frame was received, 0 if no data, -1 on error/disconnect. */
int idk_comp_recv_frame(int client_fd, struct idk_frame_hdr *out_hdr,
                        int *out_fd, const char *log_tag);

/* Send ACK to client. If size_pending && resize stable, embed w/h.
 * Flips size_pending to false after embedding. */
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
