 /*
 * compositor_egl.h - EGL/GL compositor for overlay frames
 *
 * Simpler API than before:
 *   1. idk_compositor_egl_init() - connect to webview socket (main thread)
 *   2. idk_compositor_egl_init_gl() - init GL shaders/VBO (in GL context)
 *   3. idk_compositor_egl_render() - receive frame from socket (non-blocking, in swap hook)
 *   4. idk_compositor_egl_render_overlay() - render last frame as fullscreen quad
 */

#ifndef IDK_COMPOSITOR_EGL_H
#define IDK_COMPOSITOR_EGL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Connect to webview socket for receiving overlay frames.
 * Call from main thread during init.
 * @return 0 on success, -1 on failure.
 */
int idk_compositor_egl_init(void);

/**
 * Initialize GL shaders and VBO for fullscreen quad rendering.
 * Call from GL context (after GL is ready).
 * @return 0 on success, -1 on failure.
 */
int idk_compositor_egl_init_gl(void);

/**
 * Non-blocking: receive a new overlay frame from webview socket.
 * Converts to GL texture and stores it internally.
 * Call from swap hook BEFORE calling original swap.
 * @return 0 if frame received (render_overlay will work), -1 if no frame.
 */
int idk_compositor_egl_render(void);

/**
 * Render the current overlay texture as a fullscreen quad on top of game.
 * Call after idk_compositor_render() returns 0.
 * @param x, y   Top-left corner in pixels.
 * @param w, h   Width/height in pixels.
 */
void idk_compositor_egl_render_overlay(int x, int y, uint32_t w, uint32_t h);

/**
 * Notify compositor of a game surface resize.
 * Thread-safe: stores new size in globals, sent with next ACK.
 */
void idk_compositor_egl_notify_resize(int w, int h);

/**
 * Check if the compositor is initialized (listening socket open).
 */
int idk_compositor_egl_has_overlay(void);

/**
 * Shut down the compositor. Destroys GL resources.
 */
void idk_compositor_egl_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_COMPOSITOR_H */
