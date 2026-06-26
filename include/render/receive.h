/*
 * receive.h — Receive dmabuf/SHM frames from external process
 *
 * Functions in this module receive frames via Unix domain socket (SCM_RIGHTS).
 * They are used by the injected process to receive dmabuf from the webview
 * process, then render overlay onto them.
 */
#ifndef IDK_RECEIVE_H
#define IDK_RECEIVE_H

#include <stddef.h>
#include <stdint.h>

#include "public/idk_ipc.h"  /* idk_frame_header_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Frame header ─────────────────────────────────────────────────────── */
/* idk_frame_header_t (24 bytes) is defined in include/public/idk_ipc.h.
 * The old 32-byte struct frame_hdr has been replaced. */

/* ── Frame reception ──────────────────────────────────────────────────── */

/**
 * Receive one frame (dmabuf or SHM fd) from the connected socket.
 * Blocks up to 2 seconds. The fd is passed via SCM_RIGHTS and must be
 * closed by the caller.
 *
 * @param sock_fd    Connected socket fd.
 * @param hdr        Output: frame header (24 bytes).
 * @param out_fd     Output: received dmabuf/SHM fd (must be closed after use).
 * @return           0 on success, -1 on EOF/error/timeout.
 */
int receive_frame(int sock_fd, idk_frame_header_t *hdr, int *out_fd);

/* ── Overlay rendering helpers ────────────────────────────────────────── */

/**
 * Render ASCII text onto ABGR8888 framebuffer.
 *
 * @param pixels   Pixel buffer (mmap'd dmabuf or SHM).
 * @param stride   Number of bytes per row (width * 4 for ABGR8888).
 * @param width    Framebuffer width in pixels.
 * @param height   Framebuffer height in pixels.
 * @param text     Null-terminated string to render.
 * @param x        Top-left X coordinate (in pixels).
 * @param y        Top-left Y coordinate (in pixels).
 * @param r        Red component (0-255).
 * @param g        Green component (0-255).
 * @param b        Blue component (0-255).
 * @param a        Alpha component (0-255).
 */
void render_text(char *pixels, size_t stride,
                 uint32_t width, uint32_t height,
                 const char *text, int x, int y,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/**
 * Render a clock onto ABGR8888 framebuffer.
 *
 * @param pixels   Pixel buffer (mmap'd dmabuf or SHM).
 * @param stride   Number of bytes per row (width * 4 for ABGR8888).
 * @param width    Framebuffer width in pixels.
 * @param height   Framebuffer height in pixels.
 * @param x        Top-left X coordinate.
 * @param y        Top-left Y coordinate.
 * @param r,g,b,a  Text color.
 */
void render_clock(char *pixels, size_t stride,
                  uint32_t width, uint32_t height,
                  int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/**
 * Render a complete overlay onto ABGR8888 framebuffer.
 *
 * This is the main entry point for the overlay rendering pipeline.
 * It renders a title bar, clock, and other UI elements onto the
 * provided framebuffer.
 *
 * @param pixels   Pixel buffer (mmap'd dmabuf or SHM).
 * @param stride   Number of bytes per row (width * 4 for ABGR8888).
 * @param width    Framebuffer width in pixels.
 * @param height   Framebuffer height in pixels.
 */
void render_overlay(char *pixels, size_t stride,
                    uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* IDK_RECEIVE_H */
