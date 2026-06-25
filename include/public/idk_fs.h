/*
 * idk_fs.h — Client module for sending overlay frames to idk-overlay socket
 *
 * Adapted from imgoverlay client protocol. Translates the imgoverlay
 * message model (create/update/destroy images) into idk-overlay's
 * wire format (frame header + fd via SCM_RIGHTS).
 *
 * Wire format (idk-overlay schema):
 *   [header: 32 bytes]          [scm_rights: 1 fd]
 *   +------------------+
 *   | width    uint32  |   ← overlay width
 *   | height   uint32  |   ← overlay height
 *   | stride   uint32  |   ← X position (overloaded for compatibility)
 *   | format   uint32  |   ← Y position (overloaded for compatibility)
 *   | num_planes uint32|   ← overlay ID (0 = single/default)
 *   | pid      uint32  |   ← pixel byte size (w * h * 4)
 *   | reserved uint32  |   ← visibility flag (1 = visible, 0 = hidden)
 *   | checksum uint32  |
 *   +------------------+
 *
 * Usage:
 *   idk_fs_init("/tmp/idk-overlay-1234", true);   // connect
 *   idk_fs_send_frame(fd, width, height, x, y, id, visible); // send
 *   idk_fs_shutdown();                             // disconnect
 */
#ifndef IDK_FS_H
#define IDK_FS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ────────────────────────────────────────────────────────── */

#define IDK_CLIENT_MAX_OVERLAYS  16
#define IDK_CLIENT_FORMAT_ABGR   0x34325258  /* DRM_FORMAT_ABGR8888 */
#define IDK_CLIENT_PIXEL_SIZE(w, h)  ((w) * (h) * 4)

/* Frame type — determines how compositor imports the fd */
#define IDK_FRAME_TYPE_DMABUF  0   /* fd is a real DMA-BUF (GPU buffer) */
#define IDK_FRAME_TYPE_SHM     1   /* fd is a memfd/SHM with raw RGBA8888 pixels */

/* ── Frame info for client ────────────────────────────────────────────── */

/**
 * Frame metadata for overlay frames sent from a client process.
 * Maps to idk-overlay's frame header with position info in stride/format.
 */
typedef struct idk_fs_frame {
    uint32_t width;       /* Frame width in pixels */
    uint32_t height;      /* Frame height in pixels */
    uint32_t stride;      /* Stride in bytes (for DMABUF) or 0 (for SHM) */
    uint32_t format;      /* DRM fourcc (for DMABUF) or 0 (for SHM) */
    uint8_t  id;          /* Overlay ID (1-based, for client bookkeeping) */
    uint8_t  visible;     /* Visibility flag */
    uint8_t  nfd;         /* Number of file descriptors to send */
    uint8_t  type;        /* Frame type: IDK_FRAME_TYPE_DMABUF or IDK_FRAME_TYPE_SHM */
    uint64_t modifier;    /* DMABUF layout modifier (0 = linear, 0xFFFF...F = invalid) */
} idk_fs_frame_t;

/* ── Client initialization ────────────────────────────────────────────── */

/**
 * Initialize the client — connect to the idk-overlay server socket.
 *
 * @param sockpath   Socket path (e.g., "/tmp/idk-overlay-1234").
 * @param reuse_fd   If true, reuses an existing fd; if false, creates new.
 * @return           0 on success, -1 on failure.
 *
 * Blocks up to ~3s (30 retries × 100ms) waiting for the server to come up.
 */
int idk_fs_init(const char *sockpath, int reuse_fd);

/**
 * Variant with configurable retry count.
 *
 * @param retries    Number of retries after the first attempt.
 *                   0 = single attempt (non-blocking, for use in event loops).
 *                   30 = ~3s total (legacy behavior, blocks the caller).
 * @return           0 on success, -1 on failure.
 */
int idk_fs_init2(const char *sockpath, int reuse_fd, int retries);

/**
 * Get the connected socket fd. Returns -1 if not connected.
 */
int idk_fs_get_fd(void);

/**
 * Shut down the client and close the socket.
 */
void idk_fs_shutdown(void);

/* ── Sending frames ───────────────────────────────────────────────────── */

/**
 * Send a frame (pixel data + fd) to the overlay socket.
 *
 * @param fd         File descriptor carrying the pixel data (SHM or dmabuf).
 * @param frame      Frame metadata (position, size, visibility).
 * @return           0 on success, -1 on failure.
 */
int idk_fs_send_frame(int fd, const idk_fs_frame_t *frame);

/**
 * Send a raw pixel buffer as a frame (creates SHM internally).
 */
int idk_fs_send_pixels(const void *pixels, const idk_fs_frame_t *frame);

/**
 * Wait for compositor ACK after sending a frame.
 * Blocks until the compositor has finished processing the frame,
 * providing flow control that syncs client frame rate to game swap rate.
 * Returns 0 on ACK received, -1 on error.
 */
int idk_fs_wait_ack(void);

/**
 * Send DMA-BUF fds directly (no SHM copy — for GPU-rendered content).
 * Supports multi-plane DMA-BUF via frame->nfd.
 *
 * @param dma_buf_fds  Array of DMA-BUF fds from GPU (Qt RHI, EGL, etc.).
 * @param frame        Frame metadata (position, size, visibility, nfd, strides, offsets, modifier).
 * @return             0 on success, -1 on failure.
 */
int idk_fs_send_dma_buf(const int *dma_buf_fds, const idk_fs_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* IDK_FS_H */
