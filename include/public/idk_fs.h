/*
 * idk_fs.h — Client module for sending overlay frames to idk-overlay socket
 *
 * Wire format (24 bytes header + 1 fd via SCM_RIGHTS):
 *   +----------------------+
 *   | modifier uint64      |  offset  0 — DRM modifier (0=linear or SHM)
 *   | width     uint32     |  offset  8
 *   | height    uint32     |  offset 12
 *   | stride    uint32     |  offset 16 — bytes per row (DMABUF), 0=SHM
 *   | flags     uint8      |  offset 20 — bit0=visible, bit1=dmabuf
 *   | _pad     uint8[3]    |  offset 21
 *   +----------------------+
 *
 * Usage:
 *   idk_fs_init("/tmp/idk-overlay-1234", false);
 *   idk_fs_send_dma_buf(fds, &frame);
 *   idk_fs_shutdown();
 */
#ifndef IDK_FS_H
#define IDK_FS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "public/idk_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ────────────────────────────────────────────────────────── */

#define IDK_CLIENT_PIXEL_SIZE(w, h)  ((w) * (h) * 4)

/* ── Frame info for client (maps 1:1 to wire header) ──────────────────── */

typedef struct idk_fs_frame {
    uint64_t modifier;  /* DRM modifier (0=linear, 0xFFFF...F=invalid)        */
    uint32_t width;     /* Frame width in pixels                              */
    uint32_t height;    /* Frame height in pixels                             */
    uint32_t stride;    /* Stride in bytes (DMABUF) or 0 (SHM)               */
    uint32_t fourcc;    /* DRM fourcc (DMABUF) or 0 (SHM)                    */
    uint8_t  flags;     /* IDK_FRAME_FLAG_* bitmask                          */
    uint8_t  nfd;       /* Number of fds to send (1 for single-plane)        */
} idk_fs_frame_t;

/* Build the wire header from the client-facing frame struct.
 * Copies all fields and zeroes padding. */
void build_frame_hdr(const idk_fs_frame_t *frame, idk_frame_header_t *hdr);

/* ── Client initialization ────────────────────────────────────────────── */

/**
 * Initialize the client — connect to the idk-overlay server socket.
 * Non-blocking: delegates to transport layer which handles retries.
 *
 * @param sockpath   Socket path (e.g., "/tmp/idk-overlay-1234").
 * @return           0 on success, -1 on failure.
 */
int idk_fs_init(const char *sockpath);

/**
 * Shut down the client and close the socket.
 */
void idk_fs_shutdown(void);

/* ── Sending frames ───────────────────────────────────────────────────── */

/**
 * Send a frame (fd + metadata) to the overlay socket.
 *
 * @param fd         File descriptor carrying pixel data (SHM or dmabuf).
 * @param frame      Frame metadata (caller fills width/height/stride/flags/etc).
 * @return           0 on success, -1 on failure.
 */
int idk_fs_send_frame(int fd, const idk_fs_frame_t *frame);

/**
 * Send a raw pixel buffer as a frame (creates SHM internally).
 * Sets frame->flags = IDK_FRAME_FLAG_VISIBLE (clears DMABUF bit).
 */
int idk_fs_send_pixels(const void *pixels, const idk_fs_frame_t *frame);

/**
 * Wait for compositor ACK after sending a frame.
 * Fills *ack with the ACK message (includes resize info).
 * @param ack        Output: ACK message (w/h for resize, ack for accept/reject).
 * @param timeout_ms Timeout in milliseconds.
 * @return           0 on ACK received, -1 on timeout/error.
 */
int idk_fs_wait_ack(idk_ack_msg_t *ack, int timeout_ms);

/**
 * Receive a REQUEST from the compositor (non-blocking poll).
 * @param req        Output: REQUEST message (type field).
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking).
 * @return           0 on REQUEST received, -1 on timeout/error.
 */
int idk_fs_recv_request(idk_request_msg_t *req, int timeout_ms);

/**
 * Check if connected to compositor.
 */
bool idk_fs_is_connected(void);

/**
 * Send DMA-BUF fds directly (no SHM copy — for GPU-rendered content).
 * Sets IDK_FRAME_FLAG_DMABUF bit in frame->flags.
 *
 * @param dma_buf_fds  Array of DMA-BUF fds from GPU (Qt RHI, EGL, etc.).
 * @param frame        Frame metadata (nfd must match array size, max 4).
 * @return             0 on success, -1 on failure.
 */
int idk_fs_send_dma_buf(const int *dma_buf_fds, const idk_fs_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* IDK_FS_H */
