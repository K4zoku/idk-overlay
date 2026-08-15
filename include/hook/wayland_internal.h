#ifndef IDK_WAYLAND_INTERNAL_H
#define IDK_WAYLAND_INTERNAL_H

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/log.h"
#include "hook/hook_util.h"
#include "hook/keycodes.h"
#include "hook/wayland_input.h"
#include "hook/wayland_input_types.h"
#include "hook/wl_fns.h"
#include "hook/wl_offsets.h"
#include "hook/wl_xkb.h"
#include "public/idk_ipc.h"

/* Logging */
#define WLOG(fmt, ...) IDK_LOG("wl-input", fmt "\n", ##__VA_ARGS__)
#define WERR(fmt, ...) IDK_ERR("wl-input", fmt "\n", ##__VA_ARGS__)

#define WL_INT_TO_FIXED(i) ((wl_fixed_t)((i) * 256))

/* Forward declarations */
struct wl_compositor;
struct wl_shm;
struct wl_buffer;
struct wl_shm_pool;

/* Capture state */
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

/* Cursor/pointer globals */
extern int32_t g_cursor_x;
extern int32_t g_cursor_y;
extern uint32_t g_last_enter_serial;
extern uint32_t g_last_pointer_serial;
extern struct wl_surface *g_last_enter_surface;
extern struct wl_proxy *g_game_pointer_proxy;
extern int g_pointer_in_surface;
extern int g_game_cursor_hidden;
extern int g_pre_capture_cursor_hidden;

/* Sidecar globals */
extern struct wl_display *g_sidecar_display;
extern struct wl_event_queue *g_sidecar_queue;
extern struct wl_seat *g_sidecar_seat;
extern struct wl_keyboard *g_sidecar_keyboard;
extern struct wl_pointer *g_sidecar_pointer;
extern struct wl_proxy *g_sidecar_cursor_shape_manager;
extern struct wl_proxy *g_sidecar_compositor;
extern struct wl_proxy *g_sidecar_shm;
extern int g_sidecar_initialized;
extern int g_sidecar_ready;
extern struct wl_proxy *g_cursor_shape_device;
extern struct wl_proxy *g_shape_device_pointer_proxy;
extern uint32_t g_sidecar_pointer_enter_serial;
extern void *g_sidecar_surface;
extern wl_fixed_t g_sidecar_sx;
extern wl_fixed_t g_sidecar_sy;
void idk_wayland_cursor_dispatch(void);
void idk_wayland_cursor_shutdown(void);
void idk_wayland_cursor_capture_changed(int captured);

/* Socket globals */
extern int g_input_listen_fd;
extern int g_client_fd;
extern int g_accept_thread_started;

/* Proxy scan globals */
extern __thread int g_in_dispatch;
extern struct wl_proxy *g_intercepted_proxies[];
extern int g_intercepted_count;

/* Listener wrapper vtables (extern - checked by proxy scan) */
extern const struct wl_pointer_listener g_ptr_wrapper;
extern const struct wl_keyboard_listener g_kb_wrapper;

/* Internal function forward declarations */
int init_input_socket(void);
void teardown_input_socket(void);
void teardown_xkb(void);

/* Socket send helpers */
void send_event_to_webview(const idk_input_event_t *ev);
void send_capture_state(uint32_t capture);
void send_overlay_state(uint8_t visible);
void send_repeat_info(void);

/* Sidecar */
int sidecar_init(struct wl_display *display);
void sidecar_ensure_cursor_shape_device(struct wl_pointer *p);
void my_wp_cursor_shape_device_set_shape(struct wl_proxy *device, uint32_t serial, uint32_t shape);
void my_wl_pointer_set_cursor(struct wl_proxy *p, uint32_t serial, struct wl_surface *surface, int32_t hx, int32_t hy);

/* Proxy scan */
void scan_and_intercept_input_proxies(struct wl_display *display);
void *direct_overwrite_implementation(struct wl_proxy *proxy, void *new_impl, void *new_data, void **old_data_out);

/* Keyboard + xkb helpers (used by both kb.c and sidecar.c) */
void update_mod_bitmask(void);
int is_capture_hotkey(uint32_t key, uint32_t keysym);
int is_overlay_hotkey(uint32_t key, uint32_t keysym);
void configure_hotkey(void);

/* Syringe orig pointers + hook targets (set by init, used in respective modules) */
extern int (*orig_wl_proxy_add_listener)(struct wl_proxy *, void (**)(void), void *);
extern int (*orig_wl_proxy_add_dispatcher)(struct wl_proxy *,
                                           int (*)(const void *, void *, uint32_t, const void *, const void *),
                                           const void *, void *);
extern struct wl_display *(*orig_wl_display_connect)(const char *name);
extern struct wl_display *(*orig_wl_display_connect_to_fd)(int fd);
extern int (*orig_wl_display_dispatch_queue_pending)(struct wl_display *, struct wl_event_queue *);
extern struct wl_proxy *(*orig_wl_proxy_marshal_array_flags)(struct wl_proxy *, uint32_t, const struct wl_interface *,
                                                             uint32_t, uint32_t, union wl_argument *);
extern void (*orig_wl_proxy_destroy)(struct wl_proxy *);

int hook_wl_proxy_add_listener(struct wl_proxy *proxy, void (**impl)(void), void *data);
int hook_wl_proxy_add_dispatcher(struct wl_proxy *proxy,
                                 int (*disp)(const void *, void *, uint32_t, const void *, const void *),
                                 const void *impl, void *data);
struct wl_display *hook_wl_display_connect(const char *name);
struct wl_display *hook_wl_display_connect_to_fd(int fd);
int hook_wl_display_dispatch_queue_pending(struct wl_display *display, struct wl_event_queue *queue);
struct wl_proxy *hook_wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
                                                   const struct wl_interface *interface, uint32_t version,
                                                   uint32_t flags, union wl_argument *args);
void hook_wl_proxy_destroy(struct wl_proxy *proxy);

#ifdef IDK_HAVE_VK_LAYER
int idk_vk_layer_is_active(void);
#else
static inline int idk_vk_layer_is_active(void) { return 0; }
#endif

#endif /* IDK_WAYLAND_INTERNAL_H */
