#ifndef IDK_WAYLAND_INTERNAL_H
#define IDK_WAYLAND_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <poll.h>

#include "hook/wayland_input.h"
#include "hook/wayland_input_types.h"
#include "hook/hook_util.h"
#include "hook/keycodes.h"
#include "public/idk_ipc.h"
#include "core/log.h"

/* Logging */
#define WLOG(fmt, ...) IDK_LOG("wl-input", fmt "\n", ##__VA_ARGS__)
#define WERR(fmt, ...) IDK_ERR("wl-input", fmt "\n", ##__VA_ARGS__)

#define WL_INT_TO_FIXED(i) ((wl_fixed_t)((i) * 256))

/* Forward declarations */
struct wl_compositor;
struct wl_shm;
struct wl_buffer;
struct wl_shm_pool;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const void *methods;
    int event_count;
    const void *events;
};

union wl_argument {
    int32_t i;
    uint32_t u;
    wl_fixed_t f;
    const char *s;
    void *o;
    int32_t h;
    void *a;
    uint32_t n;
};

struct wl_list { struct wl_list *prev; struct wl_list *next; };

/* Wayland proxy type definitions — X-macro pattern */
#define WL_FOREACH(F) \
    F(int, wl_proxy_add_listener, (struct wl_proxy *, void (**)(void), void *)) \
    F(int, wl_proxy_add_dispatcher, (struct wl_proxy *, \
        int (*)(const void *, void *, uint32_t, const void *, const void *), \
        const void *, void *)) \
    F(const char *, wl_proxy_get_class, (struct wl_proxy *)) \
    F(const void *, wl_proxy_get_listener, (struct wl_proxy *)) \
    F(struct wl_event_queue *, wl_display_create_queue, (struct wl_display *)) \
    F(struct wl_proxy *, wl_proxy_create_wrapper, (struct wl_proxy *)) \
    F(void, wl_proxy_wrapper_destroy, (struct wl_proxy *)) \
    F(void, wl_proxy_set_queue, (struct wl_proxy *, struct wl_event_queue *)) \
    F(int, wl_display_roundtrip_queue, (struct wl_display *, struct wl_event_queue *)) \
    F(int, wl_display_dispatch_queue_pending, (struct wl_display *, struct wl_event_queue *)) \
    F(void, wl_event_queue_destroy, (struct wl_event_queue *)) \
    F(uint32_t, wl_proxy_get_version, (struct wl_proxy *)) \
    F(void, wl_proxy_destroy, (struct wl_proxy *)) \
    F(struct wl_proxy *, wl_proxy_marshal_constructor_versioned, \
        (struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, ...)) \
    F(struct wl_proxy *, wl_proxy_marshal_flags, \
        (struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, ...)) \
    F(struct wl_proxy *, wl_proxy_marshal_array_flags, \
        (struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, union wl_argument *))

#define WL_TYPEDEF(ret, name, params) typedef ret (*name##_fn) params;
WL_FOREACH(WL_TYPEDEF)
#undef WL_TYPEDEF

/* xkbcommon typedefs + externs — X-macro pattern */
#define XKB_FOREACH(F) \
    F(struct xkb_context *, xkb_context_new, (int flags)) \
    F(void, xkb_context_unref, (struct xkb_context *)) \
    F(struct xkb_keymap *, xkb_keymap_new_from_string, (struct xkb_context *, char *, int, int)) \
    F(void, xkb_keymap_unref, (struct xkb_keymap *)) \
    F(struct xkb_state *, xkb_state_new, (struct xkb_keymap *)) \
    F(void, xkb_state_unref, (struct xkb_state *)) \
    F(int, xkb_state_update_key, (struct xkb_state *, uint32_t, int)) \
    F(uint32_t, xkb_state_key_get_one_sym, (struct xkb_state *, uint32_t)) \
    F(int, xkb_state_update_mask, (struct xkb_state *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)) \
    F(uint32_t, xkb_state_serialize_mods, (struct xkb_state *, int)) \
    F(int, xkb_state_mod_index_is_active, (struct xkb_state *, uint32_t, int)) \
    F(uint32_t, xkb_keymap_mod_get_index, (struct xkb_keymap *, const char *)) \
    F(uint32_t, xkb_keysym_from_name, (const char *, int))

#define XKB_TYPEDEF(ret, name, params) typedef ret (*name##_fn) params;
XKB_FOREACH(XKB_TYPEDEF)
#undef XKB_TYPEDEF

/* Wrapper state structs */
struct ptr_state {
    const struct wl_pointer_listener *game;
    void *game_data;
};

struct kb_state {
    const struct wl_keyboard_listener *game;
    void *game_data;
};

/* Opcode constants */
#define WL_DISPLAY_GET_REGISTRY          1
#define WL_REGISTRY_BIND                 0
#define WL_SEAT_GET_POINTER              0
#define WL_SEAT_GET_KEYBOARD             1
#define WP_CURSOR_SHAPE_DEVICE_SET_SHAPE 1
#define WL_POINTER_SET_CURSOR            0
#define WP_CURSOR_SHAPE_DEFAULT          1
#define WP_CURSOR_SHAPE_CROSSHAIR        8
#define WL_MARSHAL_FLAG_DESTROY          1

#define WL_EVENT_QUEUE_PROXY_LIST_OFFSET 16
#define WL_PROXY_QUEUE_LINK_OFFSET       80
#define MAX_INTERCEPTED                  16

/* xkbcommon constants */
#define IDK_XKB_CONTEXT_NO_FLAGS         0
#define IDK_XKB_KEYMAP_FORMAT_TEXT_V1    1
#define IDK_XKB_KEYMAP_COMPILE_NO_FLAGS  0
#define IDK_XKB_KEY_DOWN                 1
#define IDK_XKB_KEY_UP                   2
#define IDK_XKB_STATE_MODS_DEPRESSED     (1 << 0)
#define IDK_XKB_STATE_MODS_LATCHED       (1 << 1)
#define IDK_XKB_STATE_MODS_LOCKED        (1 << 2)
#define IDK_XKB_STATE_MODS_EFFECTIVE     (1 << 3)
#define IDK_XKB_KEYSYM_NO_FLAGS          0

/* Wayland protocol constants */
#define WL_KEYBOARD_KEY_STATE_RELEASED   0u
#define WL_KEYBOARD_KEY_STATE_PRESSED    1u
#define WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 1u
#define WL_POINTER_AXIS_VERTICAL_SCROLL  0u
#define WL_POINTER_AXIS_HORIZONTAL_SCROLL 1u
#define WL_SEAT_CAPABILITY_KEYBOARD      2
#define WL_SEAT_CAPABILITY_POINTER       1

#define IDK_XKB_KEY_Tab  0xff09
#define IDK_XKB_KEY_F1   0xffbe
#define IDK_XKB_KEY_F2   0xffbf
#define IDK_XKB_KEY_F3   0xffc0
#define IDK_XKB_KEY_F4   0xffc1
#define IDK_XKB_KEY_F5   0xffc2
#define IDK_XKB_KEY_F6   0xffc3
#define IDK_XKB_KEY_F7   0xffc4
#define IDK_XKB_KEY_F8   0xffc5
#define IDK_XKB_KEY_F9   0xffc6
#define IDK_XKB_KEY_F10  0xffc7
#define IDK_XKB_KEY_F11  0xffc8
#define IDK_XKB_KEY_F12  0xffc9
#define IDK_XKB_KEY_Scroll_Lock  0xff14
#define IDK_XKB_KEY_Pause        0xff13

/* Resolved wayland function pointers */
#define WL_EXTERN(ret, name, params) extern name##_fn real_##name;
WL_FOREACH(WL_EXTERN)
#undef WL_EXTERN

extern const struct wl_interface *g_wl_seat_interface;
extern const struct wl_interface *g_wl_keyboard_interface;
extern const struct wl_interface *g_wl_registry_interface;
extern const struct wl_interface *g_wl_pointer_interface;

extern void *g_wl_handle;

/* xkbcommon function pointers */
#define XKB_EXTERN(ret, name, params) extern name##_fn fn_##name;
XKB_FOREACH(XKB_EXTERN)
#undef XKB_EXTERN

extern void *g_xkb_handle;
extern struct xkb_context *g_xkb_ctx;
extern struct xkb_keymap *g_xkb_keymap;
extern struct xkb_state  *g_xkb_state;

extern uint32_t g_mod_idx_ctrl;
extern uint32_t g_mod_idx_shift;
extern uint32_t g_mod_idx_alt;
extern uint32_t g_mod_idx_super;

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
extern int g_sidecar_initialized;
extern int g_sidecar_ready;

extern struct wl_proxy *g_cursor_shape_device;
extern struct wl_proxy *g_shape_device_pointer_proxy;

extern uint32_t g_sidecar_pointer_enter_serial;
extern void *g_sidecar_surface;
extern wl_fixed_t g_sidecar_sx;
extern wl_fixed_t g_sidecar_sy;

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
int  init_input_socket(void);
void teardown_input_socket(void);
void teardown_xkb(void);

/* Socket send helpers */
void send_event_to_webview(const idk_input_event_t *ev);
void send_capture_state(uint32_t capture);
void send_overlay_state(uint8_t visible);
void send_repeat_info(void);

/* Sidecar */
int  sidecar_init(struct wl_display *display);
void sidecar_ensure_cursor_shape_device(struct wl_pointer *p);
void my_wp_cursor_shape_device_set_shape(struct wl_proxy *device,
                                          uint32_t serial, uint32_t shape);
void my_wl_pointer_set_cursor(struct wl_proxy *p, uint32_t serial,
                               struct wl_surface *surface, int32_t hx, int32_t hy);

/* Proxy scan */
void scan_and_intercept_input_proxies(struct wl_display *display);
void *direct_overwrite_implementation(struct wl_proxy *proxy, void *new_impl,
                                       void *new_data, void **old_data_out);

/* Keyboard + xkb helpers (used by both kb.c and sidecar.c) */
void update_mod_bitmask(void);
int  is_capture_hotkey(uint32_t key, uint32_t keysym);
int  is_overlay_hotkey(uint32_t key, uint32_t keysym);
void configure_hotkey(void);

/* Syringe orig pointers + hook targets (set by init, used in respective modules) */
extern int (*orig_wl_proxy_add_listener)(struct wl_proxy *, void (**)(void), void *);
extern int (*orig_wl_proxy_add_dispatcher)(struct wl_proxy *,
    int (*)(const void *, void *, uint32_t, const void *, const void *),
    const void *, void *);
extern struct wl_display *(*orig_wl_display_connect)(const char *name);
extern struct wl_display *(*orig_wl_display_connect_to_fd)(int fd);
extern int (*orig_wl_display_dispatch_queue_pending)(struct wl_display *, struct wl_event_queue *);
extern struct wl_proxy *(*orig_wl_proxy_marshal_array_flags)(
    struct wl_proxy *, uint32_t, const struct wl_interface *,
    uint32_t, uint32_t, union wl_argument *);
extern void (*orig_wl_proxy_destroy)(struct wl_proxy *);

int hook_wl_proxy_add_listener(struct wl_proxy *proxy,
                                void (**impl)(void), void *data);
int hook_wl_proxy_add_dispatcher(struct wl_proxy *proxy,
    int (*disp)(const void *, void *, uint32_t, const void *, const void *),
    const void *impl, void *data);
struct wl_display *hook_wl_display_connect(const char *name);
struct wl_display *hook_wl_display_connect_to_fd(int fd);
int hook_wl_display_dispatch_queue_pending(struct wl_display *display,
                                            struct wl_event_queue *queue);
struct wl_proxy *hook_wl_proxy_marshal_array_flags(
    struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface,
    uint32_t version, uint32_t flags, union wl_argument *args);
void hook_wl_proxy_destroy(struct wl_proxy *proxy);

#ifdef IDK_HAVE_VK_LAYER
int idk_vk_layer_is_active(void);
#else
static inline int idk_vk_layer_is_active(void) { return 0; }
#endif

#endif /* IDK_WAYLAND_INTERNAL_H */
