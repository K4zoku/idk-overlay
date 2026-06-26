/*
 * compositor_common.h — shared code between GL and Vulkan compositors.
 *
 * Contains frame helpers, resize debounce logic, SHM mmap cache, and
 * cross-GPU dmabuf vendor detection.
 *
 * Transport layer (socket/SHM) is in "core/transport.h".
 */

#ifndef IDK_COMPOSITOR_COMMON_H
#define IDK_COMPOSITOR_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>

#include "public/idk_ipc.h"  /* idk_frame_header_t, idk_ack_msg_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Frame protocol helpers ───────────────────────────────────────────── */

static inline bool idk_frame_is_dmabuf(const idk_frame_header_t *hdr) {
    return (hdr->flags & IDK_FRAME_FLAG_DMABUF) != 0;
}

static inline bool idk_frame_is_visible(const idk_frame_header_t *hdr) {
    return (hdr->flags & IDK_FRAME_FLAG_VISIBLE) != 0;
}

/* ── Resize debounce ──────────────────────────────────────────────────── */

#define IDK_COMP_RESIZE_DEBOUNCE_MS 50

bool idk_comp_notify_resize(int *game_w, int *game_h, bool *size_pending,
                            struct timespec *last_resize_ts,
                            int w, int h, const char *log_tag);

bool idk_comp_resize_stable(const struct timespec *last_resize_ts, int debounce_ms);

/* ── Path helpers ────────────────────────────────────────────────────── */

void idk_comp_get_path(char *buf, size_t bufsz);

/* ── ACK builder ──────────────────────────────────────────────────────── */

/* Build an idk_ack_msg_t from resize state. Caller then sends via
 * idk_tp_send_ack(). Replaces the old idk_comp_send_ack() pattern. */
void idk_comp_build_ack(idk_ack_msg_t *msg, uint8_t ack,
                         int game_w, int game_h,
                         bool *size_pending,
                         const struct timespec *last_resize_ts,
                         int debounce_ms, const char *log_tag);

/* ── SHM mmap cache ───────────────────────────────────────────────────── */
/* Caches mmap'd SHM buffers by inode to avoid remapping identical fds. */

typedef struct {
    void   *map;
    size_t  size;
    ino_t   ino;
    dev_t   dev;
} idk_shm_cache_t;

static inline void idk_shm_cache_init(idk_shm_cache_t *c) {
    c->map = NULL; c->size = 0; c->ino = 0; c->dev = 0;
}

/* Map fd, reusing cached mapping if inode unchanged. */
void *idk_shm_cache_map(idk_shm_cache_t *c, int fd);

/* Unmap and reset cache. */
void idk_shm_cache_cleanup(idk_shm_cache_t *c);

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
