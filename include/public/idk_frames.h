/*
 * idk_frames.h - Frame / ACK / request wire types
 *
 * Frame (28 bytes header + fd via SCM_RIGHTS / pidfd_getfd):
 *   +----------------------+
 *   | modifier uint64      |  offset  0 - DRM modifier (0=linear, SHM=0)
 *   | width     uint32     |  offset  8
 *   | height    uint32     |  offset 12
 *   | stride    uint32     |  offset 16 - bytes per row (DMABUF), 0=SHM
 *   | fourcc    uint32     |  offset 20 - DRM fourcc (DMABUF), 0=SHM
 *   | flags     uint8      |  offset 24 - bit0=visible, bit1=dmabuf
 *   | nfd      uint8      |  offset 25 - fd count (1–4), ignored on recv
 *   | buf_id   uint16     |  offset 26 - dmabuf pool generation (0=uncached)
 *   +----------------------+  total 28 bytes
 *
 * Frame transport is handled by idk_transport API (core/transport.h).
 */
#ifndef IDK_FRAMES_H
#define IDK_FRAMES_H

#include <assert.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame header (28 bytes, sent with fds via transport) */

#define IDK_FRAME_FLAG_VISIBLE 0x01 /* bit0: overlay visible */
#define IDK_FRAME_FLAG_DMABUF 0x02  /* bit1: 1=dmabuf fd, 0=SHM fd */

#pragma pack(push, 1)
typedef struct idk_frame_header {
  uint64_t modifier;  /* offset  0 - DRM modifier (0=linear or SHM)        */
  uint32_t width;     /* offset  8 - frame width in pixels                  */
  uint32_t height;    /* offset 12 - frame height in pixels                 */
  uint32_t stride;    /* offset 16 - bytes per row (DMABUF), 0=SHM          */
  uint32_t fourcc;    /* offset 20 - DRM fourcc (DMABUF), 0=SHM             */
  uint8_t flags;      /* offset 24 - IDK_FRAME_FLAG_*                       */
  uint8_t nfd;        /* offset 25 - fd count (1–4 for send, 0 on recv)     */
  uint16_t buf_id;    /* offset 26 - dmabuf pool generation (0=uncached)     */
} idk_frame_header_t; /* total 28 bytes                                     */
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_frame_header_t) == 28, "idk_frame_header_t must be 28 bytes");
#else
_Static_assert(sizeof(idk_frame_header_t) == 28, "idk_frame_header_t must be 28 bytes");
#endif

/* ACK (16 bytes — deliberately NOT packed, natural C alignment) */

typedef struct idk_ack_msg {
  uint8_t ack;     /* 0 = accepted, 1 = rejected (DMABUF not supported) */
  int32_t w;       /* game width (0 = no resize) */
  int32_t h;       /* game height (0 = no resize) */
  uint8_t _pad[3]; /* reserved */
} idk_ack_msg_t;

/* REQUEST message (8 bytes, compositor→webview): sent after presenting
 * a frame to request the next one. */

#define IDK_REQUEST_NEXT_FRAME 0
#define IDK_REQUEST_SHUTDOWN 1

#pragma pack(push, 1)
typedef struct idk_request_msg {
  uint8_t type; /* IDK_REQUEST_* */
  uint8_t _pad[7];
} idk_request_msg_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(idk_request_msg_t) == 8, "idk_request_msg_t must be 8 bytes");
#else
_Static_assert(sizeof(idk_request_msg_t) == 8, "idk_request_msg_t must be 8 bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* IDK_FRAMES_H */
