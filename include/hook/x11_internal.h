#ifndef IDK_X11_INTERNAL_H
#define IDK_X11_INTERNAL_H

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/log.h"
#include "hook/hook_util.h"
#include "hook/keycodes.h"
#include "hook/x11_events.h"
#include "hook/x11_input.h"
#include "hook/x11_layouts.h"
#include "public/idk_ipc.h"

/* Logging */
#define XLOG(fmt, ...) IDK_LOG("x11-input", fmt "\n", ##__VA_ARGS__)
#define XERR(fmt, ...) IDK_ERR("x11-input", fmt "\n", ##__VA_ARGS__)

/* Shared globals (mirror wayland_internal.h names where applicable) */
extern _Atomic int g_captured;
extern _Atomic int g_hotkey_pressed;
extern uint32_t g_hotkey_keysym;
extern uint32_t g_hotkey_scancode;
extern uint32_t g_hotkey_mods;
extern uint32_t g_mods;
extern int32_t g_repeat_rate;
extern int32_t g_repeat_delay;

/* Overlay visibility + overlay hotkey (defined in overlay.c) */
extern _Atomic int g_overlay_visible;
extern uint32_t g_hotkey_overlay_keysym;
extern uint32_t g_hotkey_overlay_scancode;
extern uint32_t g_hotkey_overlay_mods;

/* X11-specific globals */
extern void *g_x11_handle; /* dlopen handle for libX11.so.6 */
extern int g_hook_installed;
extern Display *g_game_display; /* cached display from first XNextEvent */
extern Window g_game_window;    /* cached window from first X event */
extern Cursor g_overlay_cursor;
extern int g_cursor_grabbed; /* XGrabPointer active */

/* Input socket (shared with wayland_socket.c structure) */
extern int g_input_listen_fd;
extern int g_client_fd;
extern int g_accept_thread_started;

int init_input_socket(void);
void teardown_input_socket(void);
void send_event_to_webview(const idk_input_event_t *ev);
void send_capture_state(uint32_t capture);
void send_overlay_state(uint8_t visible);
void send_repeat_info(void);
void idk_x11_cursor_dispatch(void);
void idk_x11_cursor_shutdown(void);

/* Hotkey */
void configure_hotkey(void);
int is_capture_hotkey(uint32_t key, uint32_t keysym);
int is_overlay_hotkey(uint32_t key, uint32_t keysym);

/* Event dispatch (called from each XNextEvent-family hook) */
/* Returns 1 if the event should be swallowed (captured/hotkey),
 * 0 if it should be returned to the caller. */
int x11_dispatch_event(XEventStorage *ev);
int x11_handle_key_event(XEventStorage *ev);
int x11_handle_button_event(XEventStorage *ev);
int x11_handle_motion_event(XEventStorage *ev);
Display *idk_x11_game_display(void);

/* Internal helpers - not part of the exported symbol surface. */
#define IDK_INTERNAL __attribute__((visibility("hidden")))

IDK_INTERNAL int hook_XNextEvent(Display *, XEventStorage *);
IDK_INTERNAL int hook_XPeekEvent(Display *, XEventStorage *);
IDK_INTERNAL int hook_XCheckWindowEvent(Display *, Window, long, XEventStorage *);
IDK_INTERNAL int hook_XMaskEvent(Display *, long, XEventStorage *);
IDK_INTERNAL int hook_XCheckMaskEvent(Display *, long, XEventStorage *);
IDK_INTERNAL int hook_XCheckTypedEvent(Display *, int, XEventStorage *);
IDK_INTERNAL int hook_XCheckTypedWindowEvent(Display *, Window, int, XEventStorage *);
IDK_INTERNAL int hook_XWindowEvent(Display *, Window, long, XEventStorage *);
IDK_INTERNAL int hook_XIfEvent(Display *, XEventStorage *, void *, void *);
IDK_INTERNAL int hook_XCheckIfEvent(Display *, XEventStorage *, void *, void *);
IDK_INTERNAL int hook_XSelectInput(Display *, Window, long);
IDK_INTERNAL void x11_ensure_event_masks(Display *, Window);
IDK_INTERNAL void fill_noexpose(XEventStorage *ev, Display *dpy);

#endif /* IDK_X11_INTERNAL_H */
