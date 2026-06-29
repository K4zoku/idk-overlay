#pragma once

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>

#include "public/idk_ipc.h"
#include "core/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline bool idk_frame_is_dmabuf(const idk_frame_header_t *hdr) {
    return (hdr->flags & IDK_FRAME_FLAG_DMABUF) != 0;
}

static inline bool idk_frame_is_visible(const idk_frame_header_t *hdr) {
    return (hdr->flags & IDK_FRAME_FLAG_VISIBLE) != 0;
}

#define IDK_COMP_RESIZE_DEBOUNCE_MS 50

bool idk_comp_notify_resize(int *game_w, int *game_h, bool *size_pending,
                            struct timespec *last_resize_ts,
                            int w, int h, const char *log_tag);

bool idk_comp_resize_stable(const struct timespec *last_resize_ts, int debounce_ms);

void idk_comp_get_path(char *buf, size_t bufsz);

void idk_comp_get_runtime_dir(char *buf, size_t bufsz);

void idk_comp_get_default_socket_path(char *buf, size_t bufsz,
                                       int with_input_suffix);

void idk_comp_get_default_abstract_name(char *buf, size_t bufsz, int input);

void idk_comp_build_ack(idk_ack_msg_t *msg, uint8_t ack,
                         int game_w, int game_h,
                         bool *size_pending,
                         const struct timespec *last_resize_ts,
                         int debounce_ms, const char *log_tag);

typedef struct {
    void   *map;
    size_t  size;
    ino_t   ino;
    dev_t   dev;
} idk_shm_cache_t;

static inline void idk_shm_cache_init(idk_shm_cache_t *c) {
    c->map = NULL; c->size = 0; c->ino = 0; c->dev = 0;
}

void *idk_shm_cache_map(idk_shm_cache_t *c, int fd);

void idk_shm_cache_cleanup(idk_shm_cache_t *c);

#define IDK_DRM_MOD_VENDOR(mod) (((mod) >> 56) & 0xFF)
#define IDK_DRM_FORMAT_MOD_INVALID 0x00FFFFFFFFFFFFFFULL

uint32_t idk_vk_vendor_to_drm(uint32_t vk_vendor);

typedef struct {
    idk_transport_t    tp;
    bool               inited;

    bool               has_frame;
    idk_frame_header_t hdr;
    int                dmabuf_fd[4];
    int                nfd;
    uint16_t           dmabuf_cache_id;
    uint32_t           frame_w, frame_h;
    bool               frame_premultiplied;

    int                game_w, game_h;
    bool               size_pending;
    struct timespec    last_resize_ts;
    struct timespec    last_frame_ts;
    bool               was_hidden;
} idk_compositor_t;

extern idk_compositor_t g_comp;

int  idk_compositor_init(void);
int  idk_compositor_recv_frame(bool visible);
void idk_compositor_send_ack(uint8_t code);
void idk_compositor_send_request(void);
void idk_compositor_notify_resize(int w, int h);
int  idk_compositor_has_frame(void);
void idk_compositor_shutdown(void);

#ifdef __cplusplus
}
#endif
