/*
 * wayland_input.h — Wayland input hooking for overlay input capture
 *
 * Hooks wl_proxy_add_listener (and optionally wl_proxy_add_dispatcher)
 * to intercept the game's wl_pointer / wl_keyboard listener registration.
 * A wrapper vtable is installed in its place; the wrapper forwards events
 * to the game by default, and swallows them (forwarding to the webview
 * via IPC instead) when "input capture" mode is toggled on.
 *
 * Capture is toggled by a hotkey (default: F8, configurable via
 * IDK_TOGGLE_KEY env var). When captured:
 *   - All keyboard events go to the webview (game sees nothing).
 *   - All mouse button/motion/axis events go to the webview.
 *   - The hotkey itself is swallowed (it only toggles capture, never
 *     reaches either side).
 *
 * The input IPC socket is /tmp/idk-overlay-<pid>-input — separate from
 * the frame socket to avoid multiplexing. The webview connects to it
 * and reads idk_ipc_input_event_t messages.
 */
#ifndef IDK_WAYLAND_INPUT_H
#define IDK_WAYLAND_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install wl_proxy_add_listener hook (and wl_proxy_add_dispatcher).
 * Idempotent — safe to call multiple times.
 *
 * Should be called after libwayland-client.so.0 is loaded by the game.
 * In practice this means: from the EGL/GLX/Vulkan swap hook on first
 * call (the existing pattern in egl_hook.c / glx_hook.c).
 *
 * Also opens the input IPC socket as a server: /tmp/idk-overlay-<pid>-input
 * (or $IDK_SOCKET-input if IDK_SOCKET is set). The webview connects to it.
 *
 * @return 0 on success, -1 on failure (e.g. libwayland not available).
 */
int idk_wayland_input_init(void);

/**
 * Remove hooks and close the input IPC socket.
 * Called from idk_overlay_shutdown().
 */
void idk_wayland_input_shutdown(void);

/**
 * Query the current capture state.
 * @return 1 if input is being captured (forwarded to webview), 0 otherwise.
 */
int idk_wayland_input_is_captured(void);

/**
 * Programmatically set the capture state. Useful for tests or for
 * triggering capture from elsewhere in the overlay (e.g. clicking a
 * "lock input" button in the overlay UI itself).
 *
 * @param enable  1 to enable capture, 0 to disable.
 */
void idk_wayland_input_set_capture(int enable);

/**
 * Dispatch pending sidecar wayland input events. Should be called from
 * the game's main thread (e.g. from the EGL swap hook) to process
 * keyboard events on the sidecar's private event queue.
 *
 * This is the MangoHud-style fallback for hotkey detection: even if
 * listener substitution misses (game registered listeners before our
 * hook installed), the sidecar still receives duplicate key events
 * and can detect the hotkey.
 */
void idk_wayland_input_sidecar_dispatch(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_WAYLAND_INPUT_H */
