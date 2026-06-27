/*
 * x11_input.h — X11 input hooking for overlay input capture
 *
 * Hooks XNextEvent / XPeekEvent / XCheckMaskEvent family to intercept
 * the game's X11 event loop. When "input capture" mode is toggled on,
 * keyboard/mouse events are swallowed from the game and forwarded to
 * the webview via IPC (idk_input_event_t on /tmp/idk-overlay-<pid>-input).
 *
 * Capture is toggled by a hotkey (default: Shift+Tab, configurable via
 * IDK_HOTKEY_CAPTURE env var). The hotkey itself is always swallowed — it
 * only toggles capture, never reaches either side.
 *
 * Socket path and wire protocol are identical to the Wayland input hook
 * (wayland_input.h) — the webview side needs no changes. Wayland and X11
 * hooks can coexist; whichever library is loaded installs its hook.
 */
#ifndef IDK_X11_INPUT_H
#define IDK_X11_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install XNextEvent-family hooks + open input IPC socket.
 * Idempotent — safe to call multiple times.
 *
 * Should be called after libX11.so.6 is loaded by the game. In practice
 * this means: from idk_overlay_init() (synchronous probe) and from
 * hook_install_thread() (background retry poller).
 *
 * @return 0 on success, -1 on failure (e.g. libX11 not available).
 */
int idk_x11_input_init(void);

/**
 * Remove hooks and close the input IPC socket.
 * Called from idk_overlay_shutdown().
 */
void idk_x11_input_shutdown(void);

/**
 * Query the current capture state.
 * @return 1 if input is being captured (forwarded to webview), 0 otherwise.
 */
int idk_x11_input_is_captured(void);

/**
 * Programmatically set the capture state.
 * @param enable  1 to enable capture, 0 to disable.
 */
void idk_x11_input_set_capture(int enable);

#ifdef __cplusplus
}
#endif

#endif /* IDK_X11_INPUT_H */
