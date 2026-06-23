/*
 * wayland_input.c — Wayland input hooking for overlay input capture
 *
 * Hooks wl_proxy_add_listener (and wl_proxy_add_dispatcher) to intercept
 * the game's wl_pointer / wl_keyboard listener registration. A wrapper
 * vtable is installed in its place; the wrapper forwards events to the
 * game by default, and swallows them (forwarding to the webview via IPC
 * instead) when "input capture" mode is toggled on.
 *
 * Capture is toggled by a hotkey (default: F8, configurable via
 * IDK_TOGGLE_KEY env var, parsed as an xkb keysym name like "F8",
 * "Scroll_Lock", "XF86AudioMute", etc.).
 *
 * See research/wayland-input-hooking.md for the full technique rationale.
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  WHY LISTENER SUBSTITUTION (not sidecar)
 * ─────────────────────────────────────────────────────────────────────────
 * MangoHud's wayland_keybinds.cpp creates a private event queue + binds its
 * own wl_seat + wl_keyboard to receive a DUPLICATE copy of every key event.
 * That works for passive keybind listening but CANNOT swallow events — the
 * game still sees them. For an interactive overlay that must redirect input
 * to a webview when toggled, we MUST intercept the game's own listener.
 *
 * wl_proxy_add_listener is the only registration path (the per-interface
 * wl_*_add_listener helpers are static inline wrappers around it). A listener
 * can only be set ONCE per proxy, so we substitute at registration time.
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  THREADING
 * ─────────────────────────────────────────────────────────────────────────
 * All Wayland listener callbacks fire on whatever thread calls
 * wl_display_dispatch_*() — typically the game's main thread. Our wrapper
 * callbacks run there too, so they can safely touch game data touched on
 * that thread. They MUST NOT block (the game's input loop would stall).
 *
 * The accept thread is the only other thread we spawn; it just blocks on
 * accept() and updates g_client_fd under a mutex.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <poll.h>

#include "hook/syringe_hook.h"
#include "hook/wayland_input.h"
#include "hook/wayland_input_types.h"
#include "public/idk_ipc.h"
#include "core/log.h"

/* ── libwayland-client function pointers (resolved via dlopen) ──────────── */

typedef int   (*wl_proxy_add_listener_fn)(struct wl_proxy *,
                                          void (**)(void), void *);
typedef int   (*wl_proxy_add_dispatcher_fn)(struct wl_proxy *,
                                            int (*)(const void *, void *,
                                                    uint32_t,
                                                    const void *,
                                                    const void *),
                                            const void *, void *);
typedef const char *(*wl_proxy_get_class_fn)(struct wl_proxy *);
typedef const void *(*wl_proxy_get_listener_fn)(struct wl_proxy *);

static wl_proxy_add_listener_fn    real_wl_proxy_add_listener    = NULL;
static wl_proxy_add_dispatcher_fn  real_wl_proxy_add_dispatcher  = NULL;
static wl_proxy_get_class_fn       real_wl_proxy_get_class       = NULL;
static wl_proxy_get_listener_fn    real_wl_proxy_get_listener    = NULL;

/* Sidecar (MangoHud-style) function pointers — used when listener
 * substitution misses (e.g. game already registered listeners before
 * our hook installed). The sidecar creates a private event queue, binds
 * its own wl_seat + wl_keyboard, and receives DUPLICATE key events.
 * It cannot swallow events, but it CAN detect the hotkey to toggle
 * capture mode.
 *
 * NOTE: wl_display_get_registry, wl_registry_bind, wl_registry_destroy,
 * wl_seat_get_keyboard, wl_keyboard_destroy, wl_seat_destroy are all
 * STATIC INLINE in wayland-client-protocol.h — they are NOT exported
 * symbols. We must implement them manually using wl_proxy_marshal_flags
 * and wl_proxy_marshal_constructor_versioned (which ARE exported). */

/* wl_interface struct layout (from wayland-util.h) */
struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const void *methods;  /* const struct wl_message * */
    int event_count;
    const void *events;   /* const struct wl_message * */
};

typedef struct wl_event_queue *(*wl_display_create_queue_fn)(struct wl_display *display);
typedef struct wl_proxy *(*wl_proxy_create_wrapper_fn)(struct wl_proxy *proxy);
typedef void (*wl_proxy_wrapper_destroy_fn)(struct wl_proxy *wrapper);
typedef void (*wl_proxy_set_queue_fn)(struct wl_proxy *proxy, struct wl_event_queue *queue);
typedef int (*wl_display_roundtrip_queue_fn)(struct wl_display *display, struct wl_event_queue *queue);
typedef int (*wl_display_dispatch_queue_pending_fn)(struct wl_display *display, struct wl_event_queue *queue);
typedef void (*wl_event_queue_destroy_fn)(struct wl_event_queue *queue);
typedef uint32_t (*wl_proxy_get_version_fn)(struct wl_proxy *proxy);
typedef void (*wl_proxy_destroy_fn)(struct wl_proxy *proxy);
typedef struct wl_proxy *(*wl_proxy_marshal_constructor_versioned_fn)(
    struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface,
    uint32_t version, ...);
typedef struct wl_proxy *(*wl_proxy_marshal_flags_fn)(
    struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface,
    uint32_t version, uint32_t flags, ...);

static wl_display_create_queue_fn          real_wl_display_create_queue = NULL;
static wl_proxy_create_wrapper_fn          real_wl_proxy_create_wrapper = NULL;
static wl_proxy_wrapper_destroy_fn         real_wl_proxy_wrapper_destroy = NULL;
static wl_proxy_set_queue_fn               real_wl_proxy_set_queue = NULL;
static wl_display_roundtrip_queue_fn       real_wl_display_roundtrip_queue = NULL;
static wl_display_dispatch_queue_pending_fn real_wl_display_dispatch_queue_pending = NULL;
static wl_event_queue_destroy_fn           real_wl_event_queue_destroy = NULL;
static wl_proxy_get_version_fn             real_wl_proxy_get_version = NULL;
static wl_proxy_destroy_fn                 real_wl_proxy_destroy = NULL;
static wl_proxy_marshal_constructor_versioned_fn real_wl_proxy_marshal_constructor_versioned = NULL;
static wl_proxy_marshal_flags_fn           real_wl_proxy_marshal_flags = NULL;

/* Interface structs — resolved via dlsym (exported as global variables) */
static const struct wl_interface *g_wl_seat_interface = NULL;
static const struct wl_interface *g_wl_keyboard_interface = NULL;
static const struct wl_interface *g_wl_registry_interface = NULL;

/* Wayland protocol opcodes (from wayland.xml):
 *   wl_display::sync = 0, wl_display::get_registry = 1
 *   wl_registry::bind = 0 (only request)
 *   wl_seat::get_pointer = 0, wl_seat::get_keyboard = 1, wl_seat::get_touch = 2
 */
#define WL_DISPLAY_GET_REGISTRY 1
#define WL_REGISTRY_BIND 0
#define WL_SEAT_GET_KEYBOARD 1

/* Manual implementation of static-inline wayland functions using
 * wl_proxy_marshal_flags / wl_proxy_marshal_constructor_versioned */
static struct wl_registry *my_wl_display_get_registry(struct wl_display *display) {
    if (!real_wl_proxy_marshal_constructor_versioned || !g_wl_registry_interface)
        return NULL;
    return (struct wl_registry *)real_wl_proxy_marshal_constructor_versioned(
        (struct wl_proxy *)display, WL_DISPLAY_GET_REGISTRY,
        g_wl_registry_interface, real_wl_proxy_get_version((struct wl_proxy *)display), NULL);
}

static void *my_wl_registry_bind(struct wl_registry *registry, uint32_t name,
                                  const struct wl_interface *interface, uint32_t version) {
    if (!real_wl_proxy_marshal_constructor_versioned || !interface)
        return NULL;
    /* wl_registry::bind has signature "usun" (from wayland-scanner):
     *   u = name (uint32)
     *   s = interface_name (string)  ← from interface->name
     *   u = version (uint32)
     *   n = new_id (object pointer)  ← interface struct
     *
     * wl_argument_from_va_list reads args in signature order, so variadic
     * args must be: name, interface->name, version, interface
     *
     * NOTE: version comes BEFORE interface in the variadic list!
     * (Previous attempts had wrong order → "invalid version" errors) */
    return real_wl_proxy_marshal_constructor_versioned(
        (struct wl_proxy *)registry, WL_REGISTRY_BIND,
        interface, version,
        name, interface->name, version, interface);
}

__attribute__((unused))
static void my_wl_registry_destroy(struct wl_registry *registry) {
    if (real_wl_proxy_destroy)
        real_wl_proxy_destroy((struct wl_proxy *)registry);
}

static struct wl_keyboard *my_wl_seat_get_keyboard(struct wl_seat *seat) {
    if (!real_wl_proxy_marshal_flags || !g_wl_keyboard_interface)
        return NULL;
    return (struct wl_keyboard *)real_wl_proxy_marshal_flags(
        (struct wl_proxy *)seat, WL_SEAT_GET_KEYBOARD,
        g_wl_keyboard_interface,
        real_wl_proxy_get_version((struct wl_proxy *)seat), 0, NULL);
}

__attribute__((unused))
static void my_wl_keyboard_destroy(struct wl_keyboard *kb) {
    if (real_wl_proxy_destroy)
        real_wl_proxy_destroy((struct wl_proxy *)kb);
}

__attribute__((unused))
static void my_wl_seat_destroy(struct wl_seat *seat) {
    if (real_wl_proxy_destroy)
        real_wl_proxy_destroy((struct wl_proxy *)seat);
}

/* Sidecar state */
static struct wl_display *g_sidecar_display = NULL;
static struct wl_event_queue *g_sidecar_queue = NULL;
static struct wl_seat *g_sidecar_seat = NULL;
static struct wl_keyboard *g_sidecar_keyboard = NULL;
static int g_sidecar_initialized = 0;

/* ── libxkbcommon function pointers (resolved via dlopen) ───────────────── */

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

typedef struct xkb_context *(*xkb_context_new_fn)(int flags);
typedef void (*xkb_context_unref_fn)(struct xkb_context *);
typedef struct xkb_keymap *(*xkb_keymap_new_from_string_fn)(
    struct xkb_context *, char *string, int format, int flags);
typedef void (*xkb_keymap_unref_fn)(struct xkb_keymap *);
typedef struct xkb_state *(*xkb_state_new_fn)(struct xkb_keymap *);
typedef void (*xkb_state_unref_fn)(struct xkb_state *);
typedef int (*xkb_state_update_key_fn)(struct xkb_state *, uint32_t key, int direction);
typedef uint32_t (*xkb_state_key_get_one_sym_fn)(struct xkb_state *, uint32_t key);
typedef int (*xkb_state_update_mask_fn)(struct xkb_state *,
    uint32_t depressed, uint32_t latched, uint32_t locked,
    uint32_t depressed_layout, uint32_t latched_layout, uint32_t locked_layout);
typedef uint32_t (*xkb_state_serialize_mods_fn)(struct xkb_state *, int components);
typedef int (*xkb_state_mod_index_is_active_fn)(struct xkb_state *,
                                                uint32_t idx, int components);
typedef uint32_t (*xkb_keymap_mod_get_index_fn)(struct xkb_keymap *, const char *name);
typedef uint32_t (*xkb_keysym_from_name_fn)(const char *name, int flags);

static xkb_context_new_fn             fn_xkb_context_new             = NULL;
static xkb_context_unref_fn           fn_xkb_context_unref           = NULL;
static xkb_keymap_new_from_string_fn  fn_xkb_keymap_new_from_string  = NULL;
static xkb_keymap_unref_fn            fn_xkb_keymap_unref            = NULL;
static xkb_state_new_fn               fn_xkb_state_new               = NULL;
static xkb_state_unref_fn             fn_xkb_state_unref             = NULL;
static xkb_state_update_key_fn        fn_xkb_state_update_key        = NULL;
static xkb_state_key_get_one_sym_fn   fn_xkb_state_key_get_one_sym   = NULL;
static xkb_state_update_mask_fn       fn_xkb_state_update_mask       = NULL;
static xkb_state_serialize_mods_fn    fn_xkb_state_serialize_mods    = NULL;
static xkb_state_mod_index_is_active_fn fn_xkb_state_mod_index_is_active = NULL;
static xkb_keymap_mod_get_index_fn    fn_xkb_keymap_mod_get_index    = NULL;
static xkb_keysym_from_name_fn        fn_xkb_keysym_from_name        = NULL;

/* xkbcommon constants (we don't want to include the header) */
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

/* ── Globals ────────────────────────────────────────────────────────────── */

static void *g_wl_handle  = NULL;
static void *g_xkb_handle = NULL;
static struct xkb_context *g_xkb_ctx = NULL;

static int g_hook_installed = 0;
static int g_input_listen_fd = -1;
static int g_client_fd = -1;          /* accepted webview connection */
static pthread_t g_accept_thread;
static int g_accept_thread_started = 0;
static pthread_mutex_t g_client_fd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Capture state — toggled by hotkey */
static volatile int g_captured = 0;
static int g_hotkey_pressed = 0;  /* shared hotkey guard */
static uint32_t g_hotkey_keysym = 0;  /* default set in init: XKB_KEY_F8 */
static uint32_t g_hotkey_scancode = 0; /* fallback: KEY_F8 (66) when no xkb */

/* xkb state per-process (most games have one keyboard; multi-seat
 * keyboards share the same keymap so this is fine) */
static struct xkb_keymap *g_xkb_keymap = NULL;
static struct xkb_state  *g_xkb_state  = NULL;

/* Cached mod indices (Control/Shift/Alt/Super) — looked up once on keymap */
static uint32_t g_mod_idx_ctrl  = UINT32_MAX;  /* invalid until keymap arrives */
static uint32_t g_mod_idx_shift = UINT32_MAX;
static uint32_t g_mod_idx_alt   = UINT32_MAX;
static uint32_t g_mod_idx_super = UINT32_MAX;

/* Current modifier bitmask (IDK_MOD_*) — updated on every modifiers event */
static uint32_t g_mods = 0;

/* Last known surface-local cursor position (from pointer enter/motion) */
static int32_t g_cursor_x = 0;
static int32_t g_cursor_y = 0;

/* ── Logging ────────────────────────────────────────────────────────────── */

#define WLOG(fmt, ...) IDK_LOG("wl-input", fmt "\n", ##__VA_ARGS__)
#define WERR(fmt, ...) IDK_ERR("wl-input", fmt "\n", ##__VA_ARGS__)

/* ── Symbol resolution ──────────────────────────────────────────────────── */

static int resolve_wayland_symbols(void) {
    if (g_wl_handle) return 0;

    /* RTLD_NOLOAD: don't load if not already loaded — game must use wayland */
    g_wl_handle = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!g_wl_handle)
        g_wl_handle = dlopen("libwayland-client.so.0", RTLD_NOW);
    if (!g_wl_handle) {
        WLOG("libwayland-client.so.0 not loaded — input hook disabled");
        return -1;
    }

    real_wl_proxy_add_listener   = (wl_proxy_add_listener_fn)
        dlsym(g_wl_handle, "wl_proxy_add_listener");
    real_wl_proxy_add_dispatcher = (wl_proxy_add_dispatcher_fn)
        dlsym(g_wl_handle, "wl_proxy_add_dispatcher");
    real_wl_proxy_get_class      = (wl_proxy_get_class_fn)
        dlsym(g_wl_handle, "wl_proxy_get_class");
    real_wl_proxy_get_listener   = (wl_proxy_get_listener_fn)
        dlsym(g_wl_handle, "wl_proxy_get_listener");

    if (!real_wl_proxy_add_listener || !real_wl_proxy_get_class) {
        WERR("failed to resolve core wayland symbols");
        dlclose(g_wl_handle);
        g_wl_handle = NULL;
        return -1;
    }

    /* Resolve sidecar symbols (MangoHud-style private event queue).
     * NOTE: wl_display_get_registry, wl_registry_bind, wl_registry_destroy,
     * wl_seat_get_keyboard, wl_keyboard_destroy, wl_seat_destroy are
     * static inline in wayland-client-protocol.h — NOT exported. We
     * resolve the underlying wl_proxy_marshal_* functions instead and
     * implement them manually (see my_wl_* functions above). */
    real_wl_display_create_queue = (wl_display_create_queue_fn)
        dlsym(g_wl_handle, "wl_display_create_queue");
    real_wl_proxy_create_wrapper = (wl_proxy_create_wrapper_fn)
        dlsym(g_wl_handle, "wl_proxy_create_wrapper");
    real_wl_proxy_wrapper_destroy = (wl_proxy_wrapper_destroy_fn)
        dlsym(g_wl_handle, "wl_proxy_wrapper_destroy");
    real_wl_proxy_set_queue = (wl_proxy_set_queue_fn)
        dlsym(g_wl_handle, "wl_proxy_set_queue");
    real_wl_display_roundtrip_queue = (wl_display_roundtrip_queue_fn)
        dlsym(g_wl_handle, "wl_display_roundtrip_queue");
    real_wl_display_dispatch_queue_pending = (wl_display_dispatch_queue_pending_fn)
        dlsym(g_wl_handle, "wl_display_dispatch_queue_pending");
    real_wl_event_queue_destroy = (wl_event_queue_destroy_fn)
        dlsym(g_wl_handle, "wl_event_queue_destroy");
    real_wl_proxy_get_version = (wl_proxy_get_version_fn)
        dlsym(g_wl_handle, "wl_proxy_get_version");
    real_wl_proxy_destroy = (wl_proxy_destroy_fn)
        dlsym(g_wl_handle, "wl_proxy_destroy");
    real_wl_proxy_marshal_constructor_versioned = (wl_proxy_marshal_constructor_versioned_fn)
        dlsym(g_wl_handle, "wl_proxy_marshal_constructor_versioned");
    real_wl_proxy_marshal_flags = (wl_proxy_marshal_flags_fn)
        dlsym(g_wl_handle, "wl_proxy_marshal_flags");

    /* Resolve interface structs — exported as global variables */
    g_wl_seat_interface = (const struct wl_interface *)
        dlsym(g_wl_handle, "wl_seat_interface");
    g_wl_keyboard_interface = (const struct wl_interface *)
        dlsym(g_wl_handle, "wl_keyboard_interface");
    g_wl_registry_interface = (const struct wl_interface *)
        dlsym(g_wl_handle, "wl_registry_interface");

    WLOG("libwayland-client resolved: add_listener=%p get_class=%p sidecar=%s",
         (void *)real_wl_proxy_add_listener, (void *)real_wl_proxy_get_class,
         (real_wl_display_create_queue && g_wl_seat_interface) ? "OK" : "MISSING");
    return 0;
}

static int resolve_xkbcommon_symbols(void) {
    if (g_xkb_handle) return 0;

    /* xkbcommon is optional — without it, hotkey detection falls back to
     * raw scancodes and keysym=0 in forwarded events. */
    g_xkb_handle = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!g_xkb_handle)
        g_xkb_handle = dlopen("libxkbcommon.so.0", RTLD_NOW);
    if (!g_xkb_handle) {
        WLOG("libxkbcommon.so.0 not available — keysym translation disabled");
        return -1;
    }

    fn_xkb_context_new             = (xkb_context_new_fn)            dlsym(g_xkb_handle, "xkb_context_new");
    fn_xkb_context_unref           = (xkb_context_unref_fn)          dlsym(g_xkb_handle, "xkb_context_unref");
    fn_xkb_keymap_new_from_string  = (xkb_keymap_new_from_string_fn) dlsym(g_xkb_handle, "xkb_keymap_new_from_string");
    fn_xkb_keymap_unref            = (xkb_keymap_unref_fn)           dlsym(g_xkb_handle, "xkb_keymap_unref");
    fn_xkb_state_new               = (xkb_state_new_fn)              dlsym(g_xkb_handle, "xkb_state_new");
    fn_xkb_state_unref             = (xkb_state_unref_fn)            dlsym(g_xkb_handle, "xkb_state_unref");
    fn_xkb_state_update_key        = (xkb_state_update_key_fn)       dlsym(g_xkb_handle, "xkb_state_update_key");
    fn_xkb_state_key_get_one_sym   = (xkb_state_key_get_one_sym_fn)  dlsym(g_xkb_handle, "xkb_state_key_get_one_sym");
    fn_xkb_state_update_mask       = (xkb_state_update_mask_fn)      dlsym(g_xkb_handle, "xkb_state_update_mask");
    fn_xkb_state_serialize_mods    = (xkb_state_serialize_mods_fn)   dlsym(g_xkb_handle, "xkb_state_serialize_mods");
    fn_xkb_state_mod_index_is_active = (xkb_state_mod_index_is_active_fn)
                                       dlsym(g_xkb_handle, "xkb_state_mod_index_is_active");
    fn_xkb_keymap_mod_get_index    = (xkb_keymap_mod_get_index_fn)   dlsym(g_xkb_handle, "xkb_keymap_mod_get_index");
    fn_xkb_keysym_from_name        = (xkb_keysym_from_name_fn)       dlsym(g_xkb_handle, "xkb_keysym_from_name");

    if (!fn_xkb_context_new || !fn_xkb_keymap_new_from_string ||
        !fn_xkb_state_new || !fn_xkb_state_key_get_one_sym) {
        WERR("libxkbcommon missing required symbols");
        dlclose(g_xkb_handle);
        g_xkb_handle = NULL;
        return -1;
    }

    g_xkb_ctx = fn_xkb_context_new(IDK_XKB_CONTEXT_NO_FLAGS);
    if (!g_xkb_ctx) {
        WERR("xkb_context_new failed");
        return -1;
    }
    WLOG("libxkbcommon resolved, ctx=%p", (void *)g_xkb_ctx);
    return 0;
}

/* ── Input IPC socket (server side) ─────────────────────────────────────── */

static void *accept_thread_main(void *arg) {
    (void)arg;
    int listen_fd = g_input_listen_fd;
    while (1) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;  /* listen socket closed */
        }
        pthread_mutex_lock(&g_client_fd_lock);
        if (g_client_fd >= 0) {
            close(g_client_fd);  /* replace old connection */
        }
        g_client_fd = fd;
        pthread_mutex_unlock(&g_client_fd_lock);
        WLOG("webview connected to input socket (fd=%d)", fd);
    }
    return NULL;
}

static int init_input_socket(void) {
    /* Derive input socket path from IDK_SOCKET or PID.
     * Pattern: ${IDK_SOCKET}-input  OR  /tmp/idk-overlay-<pid>-input */
    char path[128];
    const char *base = getenv("IDK_SOCKET");
    if (base && base[0]) {
        snprintf(path, sizeof(path), "%.107s-input", base);
    } else {
        snprintf(path, sizeof(path), "/tmp/idk-overlay-%d-input", (int)getpid());
    }

    /* Clean up any stale socket */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        WERR("input socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        WERR("input bind(%s) failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        WERR("input listen() failed: %s", strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }

    g_input_listen_fd = fd;
    if (pthread_create(&g_accept_thread, NULL, accept_thread_main, NULL) != 0) {
        WERR("pthread_create failed");
        close(fd);
        unlink(path);
        g_input_listen_fd = -1;
        return -1;
    }
    g_accept_thread_started = 1;

    WLOG("input socket listening on %s", path);
    return 0;
}

/* Send an event to the connected webview (if any). Non-blocking. */
static void send_event_to_webview(const idk_ipc_input_event_t *ev) {
    pthread_mutex_lock(&g_client_fd_lock);
    int fd = g_client_fd;
    pthread_mutex_unlock(&g_client_fd_lock);
    if (fd < 0) {
        WLOG("send_event_to_webview: DROPPED (no webview connected, fd=-1)");
        return;  /* no webview connected — silently drop */
    }
    int rc = idk_ipc_send_input(fd, ev);
    if (rc != 0) {
        WLOG("send_event_to_webview: send failed (fd=%d, rc=%d, errno=%d)", fd, rc, errno);
    }
}

static void send_capture_state(uint32_t capture) {
    idk_ipc_input_event_t ev = { 0 };
    ev.type    = IDK_INPUT_STATE;
    ev.capture = capture;
    ev.mods    = g_mods;
    send_event_to_webview(&ev);
}

/* ── Capture state ──────────────────────────────────────────────────────── */

int idk_wayland_input_is_captured(void) {
    return g_captured;
}

void idk_wayland_input_set_capture(int enable) {
    int new_state = enable ? 1 : 0;
    if (new_state == g_captured) return;
    g_captured = new_state;
    WLOG("input capture %s", new_state ? "ENABLED" : "DISABLED");
    send_capture_state((uint32_t)new_state);
}

/* ── Per-proxy saved state ──────────────────────────────────────────────── */

struct ptr_state {
    const struct wl_pointer_listener *game;
    void *game_data;
};

struct kb_state {
    const struct wl_keyboard_listener *game;
    void *game_data;
};

/* ── Forward declarations of wrapper vtables ────────────────────────────── */

static void wptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                       struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy);
static void wptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                       struct wl_surface *s);
static void wptr_motion(void *d, struct wl_pointer *p, uint32_t time,
                        wl_fixed_t sx, wl_fixed_t sy);
static void wptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                        uint32_t time, uint32_t button, uint32_t state);
static void wptr_axis(void *d, struct wl_pointer *p, uint32_t time,
                      uint32_t axis, wl_fixed_t value);
static void wptr_frame(void *d, struct wl_pointer *p);
static void wptr_axis_source(void *d, struct wl_pointer *p, uint32_t src);
static void wptr_axis_stop(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis);
static void wptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t disc);

static const struct wl_pointer_listener g_ptr_wrapper = {
    .enter          = wptr_enter,
    .leave          = wptr_leave,
    .motion         = wptr_motion,
    .button         = wptr_button,
    .axis           = wptr_axis,
    .frame          = wptr_frame,
    .axis_source    = wptr_axis_source,
    .axis_stop      = wptr_axis_stop,
    .axis_discrete  = wptr_axis_discrete,
};

static void wkb_keymap(void *d, struct wl_keyboard *kb, uint32_t fmt, int32_t fd, uint32_t sz);
static void wkb_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
                      struct wl_surface *s, struct wl_array *keys);
static void wkb_leave(void *d, struct wl_keyboard *kb, uint32_t serial,
                      struct wl_surface *s);
static void wkb_key(void *d, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state);
static void wkb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial,
                          uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp);
static void wkb_repeat_info(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay);

static const struct wl_keyboard_listener g_kb_wrapper = {
    .keymap       = wkb_keymap,
    .enter        = wkb_enter,
    .leave        = wkb_leave,
    .key          = wkb_key,
    .modifiers    = wkb_modifiers,
    .repeat_info  = wkb_repeat_info,
};

/* ── Pointer wrapper callbacks ──────────────────────────────────────────── */

static void wptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                       struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
    struct ptr_state *st = (struct ptr_state *)d;
    g_cursor_x = WL_FIXED_TO_INT(sx);
    g_cursor_y = WL_FIXED_TO_INT(sy);

    /* ALWAYS forward enter/leave to the game so it can call
     * wl_pointer.set_cursor() with the enter serial — otherwise
     * the compositor hides the cursor when no surface has a cursor
     * set, and the user can't see where they're clicking.
     * Motion/button/axis are still swallowed when captured. */
    if (st->game && st->game->enter)
        st->game->enter(st->game_data, p, serial, s, sx, sy);
}

static void wptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                       struct wl_surface *s) {
    struct ptr_state *st = (struct ptr_state *)d;
    if (st->game && st->game->leave)
        st->game->leave(st->game_data, p, serial, s);
}

static void wptr_motion(void *d, struct wl_pointer *p, uint32_t time,
                        wl_fixed_t sx, wl_fixed_t sy) {
    struct ptr_state *st = (struct ptr_state *)d;
    g_cursor_x = WL_FIXED_TO_INT(sx);
    g_cursor_y = WL_FIXED_TO_INT(sy);

    if (g_captured) {
        idk_ipc_input_event_t ev = { 0 };
        ev.type   = IDK_INPUT_MOTION;
        ev.time   = time;
        ev.x      = g_cursor_x;
        ev.y      = g_cursor_y;
        ev.mods   = g_mods;
        ev.capture = 1;
        send_event_to_webview(&ev);
        return;
    }
    if (st->game && st->game->motion)
        st->game->motion(st->game_data, p, time, sx, sy);
}

static void wptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                        uint32_t time, uint32_t button, uint32_t state) {
    struct ptr_state *st = (struct ptr_state *)d;
    if (g_captured) {
        idk_ipc_input_event_t ev = { 0 };
        ev.type   = IDK_INPUT_BUTTON;
        ev.time   = time;
        ev.serial = serial;
        ev.button = button;
        ev.state  = state;
        ev.x      = g_cursor_x;
        ev.y      = g_cursor_y;
        ev.mods   = g_mods;
        ev.capture = 1;
        send_event_to_webview(&ev);
        return;
    }
    if (st->game && st->game->button)
        st->game->button(st->game_data, p, serial, time, button, state);
}

static void wptr_axis(void *d, struct wl_pointer *p, uint32_t time,
                      uint32_t axis, wl_fixed_t value) {
    struct ptr_state *st = (struct ptr_state *)d;
    if (g_captured) {
        idk_ipc_input_event_t ev = { 0 };
        ev.type   = IDK_INPUT_AXIS;
        ev.time   = time;
        /* axis 0 = vertical (dy), axis 1 = horizontal (dx) */
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            ev.dy = WL_FIXED_TO_INT(value);
        else
            ev.dx = WL_FIXED_TO_INT(value);
        ev.x      = g_cursor_x;
        ev.y      = g_cursor_y;
        ev.mods   = g_mods;
        ev.capture = 1;
        send_event_to_webview(&ev);
        return;
    }
    if (st->game && st->game->axis)
        st->game->axis(st->game_data, p, time, axis, value);
}

static void wptr_frame(void *d, struct wl_pointer *p) {
    struct ptr_state *st = (struct ptr_state *)d;
    if (g_captured) return;  /* swallow frame event too */
    if (st->game && st->game->frame)
        st->game->frame(st->game_data, p);
}

static void wptr_axis_source(void *d, struct wl_pointer *p, uint32_t src) {
    struct ptr_state *st = (struct ptr_state *)d;
    if (g_captured) return;
    if (st->game && st->game->axis_source)
        st->game->axis_source(st->game_data, p, src);
}

static void wptr_axis_stop(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis) {
    struct ptr_state *st = (struct ptr_state *)d;
    if (g_captured) return;
    if (st->game && st->game->axis_stop)
        st->game->axis_stop(st->game_data, p, time, axis);
}

static void wptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t disc) {
    struct ptr_state *st = (struct ptr_state *)d;
    if (g_captured) return;
    if (st->game && st->game->axis_discrete)
        st->game->axis_discrete(st->game_data, p, axis, disc);
}

/* ── Keyboard wrapper callbacks ─────────────────────────────────────────── */

static void wkb_keymap(void *d, struct wl_keyboard *kb, uint32_t fmt, int32_t fd, uint32_t sz) {
    struct kb_state *st = (struct kb_state *)d;

    /* The wayland spec says the client MUST close the fd after consuming it.
     * We need our own copy to build an xkb keymap, so dup() BEFORE forwarding
     * to the game (the game will close the original per spec). */
    int dup_fd = -1;
    if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && g_xkb_handle && g_xkb_ctx) {
        dup_fd = dup(fd);
        if (dup_fd < 0) {
            WLOG("keymap dup() failed: %s — xkb disabled", strerror(errno));
        }
    }

    /* Always forward keymap to game first — it needs it for its own input
     * handling (and it owns the fd per spec). The game closes the fd. */
    if (st->game && st->game->keymap) {
        st->game->keymap(st->game_data, kb, fmt, fd, sz);
    }

    /* Build our own xkb keymap + state from the dup'd fd */
    if (dup_fd >= 0 && fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        void *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, dup_fd, 0);
        if (map == MAP_FAILED) {
            WLOG("keymap mmap failed: %s", strerror(errno));
        } else {
            /* Free old keymap/state if compositor re-sends keymap */
            if (g_xkb_state && fn_xkb_state_unref) {
                fn_xkb_state_unref(g_xkb_state);
                g_xkb_state = NULL;
            }
            if (g_xkb_keymap && fn_xkb_keymap_unref) {
                fn_xkb_keymap_unref(g_xkb_keymap);
                g_xkb_keymap = NULL;
            }

            g_xkb_keymap = fn_xkb_keymap_new_from_string(
                g_xkb_ctx, (char *)map,
                IDK_XKB_KEYMAP_FORMAT_TEXT_V1,
                IDK_XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (g_xkb_keymap) {
                g_xkb_state = fn_xkb_state_new(g_xkb_keymap);
                if (g_xkb_state) {
                    /* Cache mod indices for IDK_MOD_* translation.
                     * XKB mod names: Control, Shift, Mod1 (Alt), Mod4 (Super).
                     * Mod indices come from the keymap; we look them up once. */
                    if (fn_xkb_keymap_mod_get_index) {
                        g_mod_idx_ctrl  = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Control");
                        g_mod_idx_shift = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Shift");
                        g_mod_idx_alt   = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Mod1");
                        g_mod_idx_super = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Mod4");
                    }
                    WLOG("xkb keymap loaded (mods: ctrl=%u shift=%u alt=%u super=%u)",
                         g_mod_idx_ctrl, g_mod_idx_shift,
                         g_mod_idx_alt, g_mod_idx_super);
                } else {
                    WLOG("xkb_state_new failed");
                }
            } else {
                WLOG("xkb_keymap_new_from_string failed");
            }
            munmap(map, sz);
        }
        close(dup_fd);
    }
}

static void wkb_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
                      struct wl_surface *s, struct wl_array *keys) {
    struct kb_state *st = (struct kb_state *)d;
    /* Always forward enter/leave to the game so it never loses
     * Wayland keyboard focus state. Without this, the game may stop
     * receiving keyboard events after capture is toggled off. */
    if (st->game && st->game->enter)
        st->game->enter(st->game_data, kb, serial, s, keys);
}

static void wkb_leave(void *d, struct wl_keyboard *kb, uint32_t serial,
                      struct wl_surface *s) {
    struct kb_state *st = (struct kb_state *)d;
    if (st->game && st->game->leave)
        st->game->leave(st->game_data, kb, serial, s);
}

static uint32_t decode_keysym(uint32_t key) {
    /* Translate evdev scancode → xkb keysym. Key is the raw value from
     * the wayland key event (evdev scancode, no +8 offset). xkb expects
     * evdev scancode + 8. */
    if (g_xkb_state && fn_xkb_state_key_get_one_sym) {
        return fn_xkb_state_key_get_one_sym(g_xkb_state, key + 8);
    }
    return 0;  /* unknown */
}

static int is_hotkey(uint32_t key, uint32_t keysym) {
    /* Try keysym first (requires xkb), fall back to raw scancode. */
    if (g_hotkey_keysym && keysym == g_hotkey_keysym) return 1;
    if (g_hotkey_scancode && key == g_hotkey_scancode) return 1;
    return 0;
}

static void wkb_key(void *d, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state) {
    struct kb_state *st = (struct kb_state *)d;

    /* Filter out synthetic release events (key=0, state=0) — sent by
     * some compositors on keyboard leave/focus change. Would spam IPC. */
    if (key == 0 && state == 0) return;

    uint32_t keysym = decode_keysym(key);

    /* Diagnostic: log every key event (first 20 only, to avoid spam).
     * This confirms whether the keyboard hook is actually intercepting
     * events. If you DON'T see these logs when pressing keys, osu is NOT
     * using native Wayland (it's on XWayland) and needs an X11 input hook. */
    static int s_key_log_count = 0;
    if (s_key_log_count < 20) {
        s_key_log_count++;
        WLOG("wkb_key: keycode=%u keysym=0x%x state=%u (event %d)",
             key, keysym, state, s_key_log_count);
    }

    /* Update xkb state (needed for correct keysym resolution on later keys
     * AND for modifier tracking). */
    if (g_xkb_state && fn_xkb_state_update_key) {
        fn_xkb_state_update_key(g_xkb_state, key + 8,
                                state ? IDK_XKB_KEY_DOWN : IDK_XKB_KEY_UP);
    }

    if (is_hotkey(key, keysym)) {
        /* Shared hotkey guard — prevents double-toggle when both wkb_key
         * (listener substitution) and sidecar_kb_key fire for the same
         * physical key press. Only the FIRST callback to see the press
         * toggles capture. */
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED && !g_hotkey_pressed) {
            g_hotkey_pressed = 1;
            WLOG("hotkey detected (key=%u keysym=0x%x) — toggling capture", key, keysym);
            idk_wayland_input_set_capture(!g_captured);
        } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            g_hotkey_pressed = 0;
        }
        return;  /* swallow hotkey from both game and webview */
    }

    if (g_captured) {
        idk_ipc_input_event_t ev = { 0 };
        ev.type     = IDK_INPUT_KEY;
        ev.time     = time;
        ev.serial   = serial;
        ev.keycode  = key;
        ev.keysym   = keysym;
        ev.state    = state;
        ev.mods     = g_mods;
        ev.capture  = 1;
        WLOG("wkb_key SWALLOW: keycode=%u state=%u — forwarding to webview", key, state);
        send_event_to_webview(&ev);
        return;
    }
    WLOG("wkb_key FORWARD: keycode=%u state=%u — sending to game", key, state);
    if (st->game && st->game->key)
        st->game->key(st->game_data, kb, serial, time, key, state);
}

static void update_mod_bitmask(void) {
    /* Recompute IDK_MOD_* bitmask from xkb state. Called after
     * xkb_state_update_mask or xkb_state_update_key. */
    if (!g_xkb_state || !fn_xkb_state_mod_index_is_active) {
        g_mods = 0;
        return;
    }
    uint32_t m = 0;
    if (g_mod_idx_ctrl  != UINT32_MAX &&
        fn_xkb_state_mod_index_is_active(g_xkb_state, g_mod_idx_ctrl,  IDK_XKB_STATE_MODS_EFFECTIVE))
        m |= IDK_MOD_CTRL;
    if (g_mod_idx_shift != UINT32_MAX &&
        fn_xkb_state_mod_index_is_active(g_xkb_state, g_mod_idx_shift, IDK_XKB_STATE_MODS_EFFECTIVE))
        m |= IDK_MOD_SHIFT;
    if (g_mod_idx_alt   != UINT32_MAX &&
        fn_xkb_state_mod_index_is_active(g_xkb_state, g_mod_idx_alt,   IDK_XKB_STATE_MODS_EFFECTIVE))
        m |= IDK_MOD_ALT;
    if (g_mod_idx_super != UINT32_MAX &&
        fn_xkb_state_mod_index_is_active(g_xkb_state, g_mod_idx_super, IDK_XKB_STATE_MODS_EFFECTIVE))
        m |= IDK_MOD_SUPER;
    g_mods = m;
}

static void wkb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial,
                          uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) {
    struct kb_state *st = (struct kb_state *)d;

    /* Always update xkb state (needed for keysym resolution + modifier
     * tracking) — even when captured, so when capture turns off our state
     * is correct. */
    if (g_xkb_state && fn_xkb_state_update_mask) {
        fn_xkb_state_update_mask(g_xkb_state, dep, lat, lck, 0, 0, grp);
        update_mod_bitmask();
    }

    if (g_captured) return;  /* swallow from game */
    if (st->game && st->game->modifiers)
        st->game->modifiers(st->game_data, kb, serial, dep, lat, lck, grp);
}

static void wkb_repeat_info(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay) {
    struct kb_state *st = (struct kb_state *)d;
    if (g_captured) return;
    if (st->game && st->game->repeat_info)
        st->game->repeat_info(st->game_data, kb, rate, delay);
}

/* ── Solution B: Scan wl_event_queue.proxy_list for game's keyboards ────── */

/* Forward declarations — defined later in the file */
struct wl_list { struct wl_list *prev; struct wl_list *next; };
static void *direct_overwrite_implementation(struct wl_proxy *proxy,
                                              void *new_impl, void *new_data,
                                              void **old_data_out);

/*
 * When game registers its keyboard BEFORE our hook installs, the
 * add_listener hook never fires for game's keyboard. We need to find
 * it by scanning the event queue's proxy list.
 *
 * struct wl_event_queue layout (from wayland source):
 *   offset 0:  struct wl_list event_list   (prev, next = 16 bytes)
 *   offset 16: struct wl_list proxy_list   (prev, next = 16 bytes)
 *
 * struct wl_proxy layout:
 *   offset 0:  struct wl_object object (interface, impl, id = 24 bytes)
 *   ...
 *   offset 80: struct wl_list queue_link (prev, next = 16 bytes)
 *
 * struct wl_list { struct wl_list *prev; struct wl_list *next; };
 *
 * To iterate: start at proxy_list.next, walk until back to proxy_list head.
 * For each item, compute proxy = (wl_proxy *)((char *)item - 80).
 * Check wl_proxy_get_class(proxy) == "wl_keyboard" → overwrite implementation.
 *
 * We skip proxies that already have our wrapper installed (impl == g_kb_wrapper).
 */

/* wl_list offsets in structs */
#define WL_EVENT_QUEUE_PROXY_LIST_OFFSET 16
#define WL_PROXY_QUEUE_LINK_OFFSET 80

/* Track which proxies we've already intercepted to avoid double-overwrite */
#define MAX_INTERCEPTED 16
static struct wl_proxy *g_intercepted_proxies[MAX_INTERCEPTED];
static int g_intercepted_count = 0;

static int is_already_intercepted(struct wl_proxy *proxy) {
    for (int i = 0; i < g_intercepted_count; i++) {
        if (g_intercepted_proxies[i] == proxy) return 1;
    }
    return 0;
}

static void mark_intercepted(struct wl_proxy *proxy) {
    if (g_intercepted_count < MAX_INTERCEPTED) {
        g_intercepted_proxies[g_intercepted_count++] = proxy;
    }
}

static void scan_and_intercept_input_proxies(struct wl_display *display) {
    if (!real_wl_proxy_get_class || !real_wl_proxy_get_version) return;

    struct wl_proxy *display_proxy = (struct wl_proxy *)display;
    struct wl_event_queue *queue = *(struct wl_event_queue **)((char *)display_proxy + 32);
    if (!queue) return;

    /* Iterate proxy_list of this queue */
    struct wl_list *head = (struct wl_list *)((char *)queue + WL_EVENT_QUEUE_PROXY_LIST_OFFSET);
    struct wl_list *item = head->next;

    int found = 0;
    while (item != head && found < MAX_INTERCEPTED) {
        struct wl_proxy *proxy = (struct wl_proxy *)((char *)item - WL_PROXY_QUEUE_LINK_OFFSET);
        item = item->next;

        const char *cls = real_wl_proxy_get_class(proxy);
        if (!cls) continue;

        /* Check if already intercepted (our wrapper installed) */
        void **impl_ptr = (void **)((char *)proxy + 8);
        if (*impl_ptr == (void *)&g_kb_wrapper ||
            *impl_ptr == (void *)&g_ptr_wrapper) continue;
        if (is_already_intercepted(proxy)) continue;

        /* Skip our sidecar's proxies */
        if (proxy == (struct wl_proxy *)g_sidecar_keyboard) continue;

        if (strcmp(cls, "wl_keyboard") == 0) {
            struct kb_state *st = (struct kb_state *)calloc(1, sizeof(*st));
            if (!st) continue;
            void *old_data = NULL;
            void *old_impl = direct_overwrite_implementation(
                proxy, (void *)&g_kb_wrapper, st, &old_data);
            st->game = (const struct wl_keyboard_listener *)old_impl;
            st->game_data = old_data;
            mark_intercepted(proxy);
            WLOG("scan: intercepted game keyboard: proxy=%p game=%p data=%p",
                 (void *)proxy, (void *)st->game, st->game_data);
            found++;
        } else if (strcmp(cls, "wl_pointer") == 0) {
            struct ptr_state *st = (struct ptr_state *)calloc(1, sizeof(*st));
            if (!st) continue;
            void *old_data = NULL;
            void *old_impl = direct_overwrite_implementation(
                proxy, (void *)&g_ptr_wrapper, st, &old_data);
            st->game = (const struct wl_pointer_listener *)old_impl;
            st->game_data = old_data;
            mark_intercepted(proxy);
            WLOG("scan: intercepted game pointer: proxy=%p game=%p data=%p",
                 (void *)proxy, (void *)st->game, st->game_data);
            found++;
        }
    }
}

/* ── The hook on wl_proxy_add_listener ──────────────────────────────────── */

static int (*orig_wl_proxy_add_listener)(struct wl_proxy *, void (**)(void), void *) = NULL;

/* Direct implementation overwrite — bypasses wl_proxy_add_listener's
 * "already has listener" check by writing directly to the proxy struct.
 *
 * wl_proxy struct layout (from wayland source):
 *   offset 0:  const struct wl_interface *interface
 *   offset 8:  const void *implementation   ← listener vtable
 *   offset 16: uint32_t id
 *   offset 24: struct wl_display *display
 *   offset 32: struct wl_event_queue *queue
 *   offset 40: uint32_t flags
 *   offset 44: int refcount
 *   offset 48: void *user_data              ← listener data
 *
 * Returns the old implementation (game's listener) or NULL if none. */
static void *direct_overwrite_implementation(struct wl_proxy *proxy,
                                              void *new_impl, void *new_data,
                                              void **old_data_out) {
    /* implementation is at offset 8, user_data at offset 48 */
    void **impl_ptr = (void **)((char *)proxy + 8);
    void **data_ptr = (void **)((char *)proxy + 48);

    void *old_impl = *impl_ptr;
    if (old_data_out) *old_data_out = *data_ptr;

    *impl_ptr = new_impl;
    *data_ptr = new_data;

    return old_impl;
}

static int hook_wl_proxy_add_listener(struct wl_proxy *proxy,
                                       void (**impl)(void), void *data) {
    if (!real_wl_proxy_get_class || !real_wl_proxy_add_listener) {
        /* Not initialized — pass through (shouldn't happen, but defensive) */
        if (orig_wl_proxy_add_listener)
            return orig_wl_proxy_add_listener(proxy, impl, data);
        return -1;
    }

    const char *cls = real_wl_proxy_get_class(proxy);
    if (!cls) {
        /* Unknown class — pass through */
        return orig_wl_proxy_add_listener
            ? orig_wl_proxy_add_listener(proxy, impl, data)
            : real_wl_proxy_add_listener(proxy, impl, data);
    }

    /* Log only input-related classes, but only first few times to avoid
     * spam if the game registers many keyboards/pointers. */
    static int s_kb_log_count = 0;
    static int s_ptr_log_count = 0;
    static int s_seat_log_count = 0;
    if (strcmp(cls, "wl_pointer") == 0 && s_ptr_log_count < 3) {
        s_ptr_log_count++;
        WLOG("add_listener: class=%s impl=%p data=%p",
             cls, (void *)impl, data);
    } else if (strcmp(cls, "wl_keyboard") == 0 && s_kb_log_count < 3) {
        s_kb_log_count++;
        WLOG("add_listener: class=%s impl=%p data=%p",
             cls, (void *)impl, data);
    } else if (strcmp(cls, "wl_seat") == 0 && s_seat_log_count < 3) {
        s_seat_log_count++;
        WLOG("add_listener: class=%s impl=%p data=%p",
             cls, (void *)impl, data);
    }

    /* No re-entry guard needed — we use direct_overwrite_implementation
     * which writes directly to the proxy struct without calling
     * wl_proxy_add_listener, so there's no hook re-entry. */

    if (strcmp(cls, "wl_pointer") == 0) {
        struct ptr_state *st = (struct ptr_state *)calloc(1, sizeof(*st));
        if (!st) goto passthrough;
        st->game      = (const struct wl_pointer_listener *)impl;
        st->game_data = data;
        /* ALWAYS use direct overwrite — see wl_keyboard comment above */
        void *old_data = NULL;
        void *old_impl = direct_overwrite_implementation(
            proxy, (void *)&g_ptr_wrapper, st, &old_data);
        if (old_impl) {
            st->game = (const struct wl_pointer_listener *)old_impl;
            st->game_data = old_data;
        }
        WLOG("intercepted wl_pointer: game=%p data=%p proxy=%p (direct overwrite, old_impl=%p)",
             (void *)st->game, st->game_data, (void *)proxy, old_impl);
        return 0;
    }

    if (strcmp(cls, "wl_keyboard") == 0) {
        struct kb_state *st = (struct kb_state *)calloc(1, sizeof(*st));
        if (!st) goto passthrough;
        st->game      = (const struct wl_keyboard_listener *)impl;
        st->game_data = data;
        /* ALWAYS use direct overwrite — don't call real_wl_proxy_add_listener
         * because it can re-enter our hook for the SAME proxy (guard skips
         * it, but then game's listener gets installed instead of ours).
         * Direct overwrite bypasses the hook entirely. */
        void *old_data = NULL;
        void *old_impl = direct_overwrite_implementation(
            proxy, (void *)&g_kb_wrapper, st, &old_data);
        if (old_impl) {
            /* Proxy already had a listener — save it so we can forward */
            st->game = (const struct wl_keyboard_listener *)old_impl;
            st->game_data = old_data;
        }
        WLOG("intercepted wl_keyboard: game=%p data=%p proxy=%p (direct overwrite, old_impl=%p)",
             (void *)st->game, st->game_data, (void *)proxy, old_impl);
        return 0;
    }

    /* wl_touch and everything else: pass through unchanged (no logging). */

passthrough:
    return orig_wl_proxy_add_listener
        ? orig_wl_proxy_add_listener(proxy, impl, data)
        : real_wl_proxy_add_listener(proxy, impl, data);
}

/* ── Hook on wl_proxy_add_dispatcher (GTK/Qt-style clients) ─────────────── */
/*
 * For v1 we don't intercept dispatchers — we just pass through. Adding
 * dispatcher interception would require parsing the wl_message + wl_argument
 * union for every event opcode. Most games use SDL/GLFW which use the
 * listener path, so this is OK for now.
 *
 * The hook is still installed so we can LOG dispatcher usage for diagnosis.
 */

static int (*orig_wl_proxy_add_dispatcher)(struct wl_proxy *,
    int (*)(const void *, void *, uint32_t, const void *, const void *),
    const void *, void *) = NULL;

static int hook_wl_proxy_add_dispatcher(struct wl_proxy *proxy,
    int (*disp)(const void *, void *, uint32_t, const void *, const void *),
    const void *impl, void *data) {
    if (real_wl_proxy_get_class) {
        const char *cls = real_wl_proxy_get_class(proxy);
        if (cls && (strcmp(cls, "wl_pointer") == 0 || strcmp(cls, "wl_keyboard") == 0)) {
            WLOG("NOTE: %s uses dispatcher (GTK/Qt-style) — input hook "
                 "won't intercept. Listener path is required.", cls);
        }
    }
    return orig_wl_proxy_add_dispatcher
        ? orig_wl_proxy_add_dispatcher(proxy, disp, impl, data)
        : (real_wl_proxy_add_dispatcher
           ? real_wl_proxy_add_dispatcher(proxy, disp, impl, data)
           : -1);
}

/* ── Hotkey configuration ───────────────────────────────────────────────── */

/* Linux input-event-codes.h scancodes (used as fallback when xkb is unavailable) */
#define IDK_KEY_F1   59
#define IDK_KEY_F2   60
#define IDK_KEY_F3   61
#define IDK_KEY_F4   62
#define IDK_KEY_F5   63
#define IDK_KEY_F6   64
#define IDK_KEY_F7   65
#define IDK_KEY_F8   66
#define IDK_KEY_F9   67
#define IDK_KEY_F10  68
#define IDK_KEY_F11  87
#define IDK_KEY_F12  88
#define IDK_KEY_SCROLLLOCK 70
#define IDK_KEY_PAUSE      119

struct hotkey_name_to_scancode {
    const char *name;
    uint32_t scancode;
    uint32_t keysym;  /* XKB_KEY_* value */
};

/* Common XKB keysym values (from X11/keysymdef.h, subset) */
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

static const struct hotkey_name_to_scancode HOTKEY_TABLE[] = {
    { "F1",            IDK_KEY_F1,           IDK_XKB_KEY_F1  },
    { "F2",            IDK_KEY_F2,           IDK_XKB_KEY_F2  },
    { "F3",            IDK_KEY_F3,           IDK_XKB_KEY_F3  },
    { "F4",            IDK_KEY_F4,           IDK_XKB_KEY_F4  },
    { "F5",            IDK_KEY_F5,           IDK_XKB_KEY_F5  },
    { "F6",            IDK_KEY_F6,           IDK_XKB_KEY_F6  },
    { "F7",            IDK_KEY_F7,           IDK_XKB_KEY_F7  },
    { "F8",            IDK_KEY_F8,           IDK_XKB_KEY_F8  },
    { "F9",            IDK_KEY_F9,           IDK_XKB_KEY_F9  },
    { "F10",           IDK_KEY_F10,          IDK_XKB_KEY_F10 },
    { "F11",           IDK_KEY_F11,          IDK_XKB_KEY_F11 },
    { "F12",           IDK_KEY_F12,          IDK_XKB_KEY_F12 },
    { "Scroll_Lock",   IDK_KEY_SCROLLLOCK,   IDK_XKB_KEY_Scroll_Lock },
    { "Pause",         IDK_KEY_PAUSE,        IDK_XKB_KEY_Pause },
};

static void configure_hotkey(void) {
    const char *env = getenv("IDK_TOGGLE_KEY");
    if (!env || !env[0]) env = "F8";  /* default */

    /* Try xkb_keysym_from_name first (handles arbitrary keysyms) */
    if (fn_xkb_keysym_from_name) {
        uint32_t ks = fn_xkb_keysym_from_name(env, IDK_XKB_KEYSYM_NO_FLAGS);
        if (ks != 0) {
            g_hotkey_keysym = ks;
            /* Also try to find a matching scancode for fallback */
            for (size_t i = 0; i < sizeof(HOTKEY_TABLE) / sizeof(HOTKEY_TABLE[0]); i++) {
                if (strcmp(env, HOTKEY_TABLE[i].name) == 0) {
                    g_hotkey_scancode = HOTKEY_TABLE[i].scancode;
                    break;
                }
            }
            WLOG("hotkey: %s (keysym=0x%x scancode=%u)",
                 env, g_hotkey_keysym, g_hotkey_scancode);
            return;
        }
    }

    /* Fall back to built-in table */
    for (size_t i = 0; i < sizeof(HOTKEY_TABLE) / sizeof(HOTKEY_TABLE[0]); i++) {
        if (strcmp(env, HOTKEY_TABLE[i].name) == 0) {
            g_hotkey_keysym = HOTKEY_TABLE[i].keysym;
            g_hotkey_scancode = HOTKEY_TABLE[i].scancode;
            WLOG("hotkey (from table): %s (keysym=0x%x scancode=%u)",
                 env, g_hotkey_keysym, g_hotkey_scancode);
            return;
        }
    }

    /* Unknown — fall back to F8 */
    g_hotkey_keysym = IDK_XKB_KEY_F8;
    g_hotkey_scancode = IDK_KEY_F8;
    WLOG("unknown hotkey '%s', falling back to F8", env);
}

/* ── Sidecar input listener (MangoHud-style) ──────────────────────────── */
/*
 * The sidecar creates a private event queue on the game's wl_display,
 * binds its own wl_seat + wl_keyboard, and receives DUPLICATE copies of
 * all key events. This is a FALLBACK for hotkey detection when listener
 * substitution misses (e.g. game registered listeners before our hook
 * installed, or uses wl_proxy_add_dispatcher).
 *
 * The sidecar CANNOT swallow events — the game still sees them. But it
 * CAN detect the hotkey and toggle capture mode.
 */

#define WL_SEAT_CAPABILITY_KEYBOARD 2

static void sidecar_kb_keymap(void *d, struct wl_keyboard *kb,
                              uint32_t fmt, int32_t fd, uint32_t sz) {
    (void)d; (void)kb;
    int dup_fd = -1;
    if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && g_xkb_handle && g_xkb_ctx) {
        dup_fd = dup(fd);
        if (dup_fd < 0) return;
    } else {
        return;
    }

    void *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, dup_fd, 0);
    if (map == MAP_FAILED) {
        close(dup_fd);
        return;
    }

    if (g_xkb_state && fn_xkb_state_unref) {
        fn_xkb_state_unref(g_xkb_state);
        g_xkb_state = NULL;
    }
    if (g_xkb_keymap && fn_xkb_keymap_unref) {
        fn_xkb_keymap_unref(g_xkb_keymap);
        g_xkb_keymap = NULL;
    }

    g_xkb_keymap = fn_xkb_keymap_new_from_string(
        g_xkb_ctx, (char *)map,
        IDK_XKB_KEYMAP_FORMAT_TEXT_V1,
        IDK_XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (g_xkb_keymap) {
        g_xkb_state = fn_xkb_state_new(g_xkb_keymap);
        if (g_xkb_state && fn_xkb_keymap_mod_get_index) {
            g_mod_idx_ctrl  = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Control");
            g_mod_idx_shift = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Shift");
            g_mod_idx_alt   = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Mod1");
            g_mod_idx_super = fn_xkb_keymap_mod_get_index(g_xkb_keymap, "Mod4");
        }
    }
    munmap(map, sz);
    close(dup_fd);
}

static void sidecar_kb_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
                             struct wl_surface *s, struct wl_array *keys) {
    (void)d; (void)kb; (void)serial; (void)s; (void)keys;
}

static void sidecar_kb_leave(void *d, struct wl_keyboard *kb, uint32_t serial,
                             struct wl_surface *s) {
    (void)d; (void)kb; (void)serial; (void)s;
}

static void sidecar_kb_key(void *d, struct wl_keyboard *kb, uint32_t serial,
                           uint32_t time, uint32_t key, uint32_t state) {
    (void)d; (void)kb; (void)serial; (void)time;

    /* Filter synthetic release events */
    if (key == 0 && state == 0) return;

    /* Detect hotkey — game ALSO receives this event (can't swallow) */
    uint32_t keysym = 0;
    if (g_xkb_state && fn_xkb_state_key_get_one_sym) {
        keysym = fn_xkb_state_key_get_one_sym(g_xkb_state, key + 8);
    }

    static int s_log = 0;
    if (s_log < 10) {
        s_log++;
        WLOG("sidecar_kb_key: keycode=%u keysym=0x%x state=%u (event %d)",
             key, keysym, state, s_log);
    }

    if (g_xkb_state && fn_xkb_state_update_key) {
        fn_xkb_state_update_key(g_xkb_state, key + 8,
                                state ? IDK_XKB_KEY_DOWN : IDK_XKB_KEY_UP);
    }

    if (is_hotkey(key, keysym)) {
        /* Same shared guard as wkb_key — if wkb_key already handled
         * the press (g_hotkey_pressed=1), sidecar won't toggle.
         * If wkb_key didn't fire (game's keyboard not intercepted),
         * sidecar handles the toggle. */
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED && !g_hotkey_pressed) {
            g_hotkey_pressed = 1;
            WLOG("sidecar hotkey detected (key=%u keysym=0x%x) — toggling capture", key, keysym);
            idk_wayland_input_set_capture(!g_captured);
        } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            g_hotkey_pressed = 0;
        }
    }
}

static void sidecar_kb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial,
                                 uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) {
    (void)d; (void)kb; (void)serial;
    if (g_xkb_state && fn_xkb_state_update_mask) {
        fn_xkb_state_update_mask(g_xkb_state, dep, lat, lck, 0, 0, grp);
        update_mod_bitmask();
    }
}

static void sidecar_kb_repeat_info(void *d, struct wl_keyboard *kb,
                                   int32_t rate, int32_t delay) {
    (void)d; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener g_sidecar_kb_listener = {
    .keymap       = sidecar_kb_keymap,
    .enter        = sidecar_kb_enter,
    .leave        = sidecar_kb_leave,
    .key          = sidecar_kb_key,
    .modifiers    = sidecar_kb_modifiers,
    .repeat_info  = sidecar_kb_repeat_info,
};

static void sidecar_seat_capabilities(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_sidecar_keyboard) {
        g_sidecar_keyboard = my_wl_seat_get_keyboard(seat);
        if (g_sidecar_keyboard) {
            /* Use direct_overwrite to install sidecar listener — bypass
             * the add_listener hook, which would intercept our sidecar's
             * keyboard instead of letting us install our own listener.
             * (The hook can't tell the difference between game's keyboard
             * and sidecar's keyboard — both are "wl_keyboard" class.) */
            void *old_data = NULL;
            void *old_impl = direct_overwrite_implementation(
                (struct wl_proxy *)g_sidecar_keyboard,
                (void *)&g_sidecar_kb_listener, NULL, &old_data);
            WLOG("sidecar: wl_keyboard bound + listener installed (direct overwrite, old_impl=%p)",
                 old_impl);
        }
    }
}

static void sidecar_seat_name(void *d, struct wl_seat *seat, const char *name) {
    (void)d; (void)seat;
    WLOG("sidecar: seat name=%s", name ? name : "(null)");
}

static const struct wl_seat_listener g_sidecar_seat_listener = {
    .capabilities = sidecar_seat_capabilities,
    .name         = sidecar_seat_name,
};

static void sidecar_registry_global(void *d, struct wl_registry *reg,
                                    uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)reg;
    WLOG("sidecar: registry global: iface=%s name=%u ver=%u",
         iface ? iface : "(null)", name, ver);
    if (strcmp(iface, "wl_seat") == 0 && !g_sidecar_seat && g_wl_seat_interface) {
        uint32_t bind_ver = ver < 5 ? ver : 5;
        WLOG("sidecar: binding wl_seat (name=%u ver=%u)", name, bind_ver);
        g_sidecar_seat = (struct wl_seat *)my_wl_registry_bind(reg, name, g_wl_seat_interface, bind_ver);
        WLOG("sidecar: wl_seat bound → %p", (void *)g_sidecar_seat);
        if (g_sidecar_seat && real_wl_proxy_add_listener) {
            real_wl_proxy_add_listener((struct wl_proxy *)g_sidecar_seat,
                                       (void (**)(void))&g_sidecar_seat_listener, NULL);
            WLOG("sidecar: wl_seat listener installed");
        }
    }
}

static void sidecar_registry_global_remove(void *d, struct wl_registry *reg, uint32_t name) {
    (void)d; (void)reg; (void)name;
}

static const struct wl_registry_listener g_sidecar_registry_listener = {
    .global        = sidecar_registry_global,
    .global_remove = sidecar_registry_global_remove,
};

static int sidecar_init(struct wl_display *display) {
    if (g_sidecar_initialized) return 0;
    if (!display) return -1;

    /* Don't retry after first failure — if symbols are missing, they'll
     * still be missing next frame. This prevents log spam from
     * dispatch_queue_pending calling sidecar_init every frame. */
    static int s_sidecar_failed = 0;
    if (s_sidecar_failed) return -1;

    /* Check all required symbols for manual wayland function implementation */
    if (!real_wl_display_create_queue || !real_wl_proxy_create_wrapper ||
        !real_wl_proxy_wrapper_destroy || !real_wl_proxy_set_queue ||
        !real_wl_display_roundtrip_queue || !real_wl_display_dispatch_queue_pending ||
        !real_wl_proxy_get_version || !real_wl_proxy_destroy ||
        !real_wl_proxy_marshal_constructor_versioned || !real_wl_proxy_marshal_flags ||
        !g_wl_seat_interface || !g_wl_keyboard_interface || !g_wl_registry_interface) {
        s_sidecar_failed = 1;
        WLOG("sidecar: missing wayland symbols: create_queue=%p create_wrapper=%p "
             "wrapper_destroy=%p set_queue=%p roundtrip=%p dispatch=%p "
             "get_version=%p destroy=%p marshal_ctor_ver=%p marshal_flags=%p "
             "seat_iface=%p kb_iface=%p registry_iface=%p",
             (void*)real_wl_display_create_queue,
             (void*)real_wl_proxy_create_wrapper,
             (void*)real_wl_proxy_wrapper_destroy,
             (void*)real_wl_proxy_set_queue,
             (void*)real_wl_display_roundtrip_queue,
             (void*)real_wl_display_dispatch_queue_pending,
             (void*)real_wl_proxy_get_version,
             (void*)real_wl_proxy_destroy,
             (void*)real_wl_proxy_marshal_constructor_versioned,
             (void*)real_wl_proxy_marshal_flags,
             (void*)g_wl_seat_interface,
             (void*)g_wl_keyboard_interface,
             (void*)g_wl_registry_interface);
        return -1;
    }

    g_sidecar_display = display;

    WLOG("sidecar: calling wl_display_create_queue");
    g_sidecar_queue = real_wl_display_create_queue(display);
    if (!g_sidecar_queue) {
        s_sidecar_failed = 1;
        WLOG("sidecar: wl_display_create_queue failed");
        return -1;
    }
    WLOG("sidecar: queue=%p", (void *)g_sidecar_queue);

    WLOG("sidecar: calling wl_proxy_create_wrapper");
    struct wl_proxy *display_wrapper = real_wl_proxy_create_wrapper((struct wl_proxy *)display);
    if (!display_wrapper) {
        s_sidecar_failed = 1;
        WLOG("sidecar: wl_proxy_create_wrapper failed");
        return -1;
    }
    WLOG("sidecar: display_wrapper=%p", (void *)display_wrapper);
    real_wl_proxy_set_queue(display_wrapper, g_sidecar_queue);

    /* Use our manual implementation of wl_display_get_registry */
    WLOG("sidecar: calling my_wl_display_get_registry (marshal_ctor_versioned=%p, registry_iface=%p)",
         (void*)real_wl_proxy_marshal_constructor_versioned, (void*)g_wl_registry_interface);
    struct wl_registry *registry = my_wl_display_get_registry((struct wl_display *)display_wrapper);
    WLOG("sidecar: registry=%p", (void *)registry);
    real_wl_proxy_wrapper_destroy(display_wrapper);

    if (!registry) {
        s_sidecar_failed = 1;
        WLOG("sidecar: wl_display_get_registry failed");
        return -1;
    }

    WLOG("sidecar: adding registry listener");
    real_wl_proxy_add_listener((struct wl_proxy *)registry,
                               (void (**)(void))&g_sidecar_registry_listener, NULL);

    /* Mark as initialized — registry listener is installed. The actual
     * seat/keyboard binding will happen asynchronously when events are
     * dispatched on our sidecar queue.
     *
     * We CANNOT call any dispatch function here because:
     * - roundtrip_queue blocks on read_events (deadlock with SDL2's read lock)
     * - dispatch_queue_pending also internally calls prepare_read → can block
     *
     * Instead, sidecar_dispatch() is called from the EGL swap hook every
     * frame. It calls dispatch_queue_pending on our sidecar queue. The first
     * call will process the registry global event (wl_seat advertisement)
     * that's already buffered, binding our wl_seat. Subsequent calls process
     * seat capability events (binding wl_keyboard) and keyboard events.
     *
     * This is safe because:
     * 1. dispatch_queue_pending is non-blocking if no events are pending
     * 2. We only dispatch OUR queue, not the game's default queue
     * 3. The game's read lock state doesn't matter — we're not reading,
     *    just processing already-read events on our queue
     *
     * The first dispatch happens on the next EGL swap, within 1 frame. */
    g_sidecar_initialized = 1;
    WLOG("sidecar: initialized (seat=%p keyboard=%p) — will bind on next dispatch",
         (void *)g_sidecar_seat, (void *)g_sidecar_keyboard);
    return 0;
}

/* Forward declaration — defined later, after hook function */
static int (*orig_wl_display_dispatch_queue_pending)(struct wl_display *, struct wl_event_queue *) = NULL;

void idk_wayland_input_sidecar_dispatch(void) {
    if (!g_sidecar_initialized || !g_sidecar_display || !g_sidecar_queue) return;
    /* Use orig_ (bounce stub) to avoid hitting the inline trampoline
     * which would re-enter our hook. orig_ safely calls the real function
     * body without re-entering the hook. */
    if (!orig_wl_display_dispatch_queue_pending) return;
    orig_wl_display_dispatch_queue_pending(g_sidecar_display, g_sidecar_queue);
}

/* ── Hook on wl_display_connect / wl_display_connect_to_fd ─────────────── */
/*
 * SDL2 on Wayland uses wl_display_connect_to_fd() (not wl_display_connect)
 * because SDL opens the socket itself via SOCK_NONBLOCK + connect(). We
 * must hook BOTH variants to catch all clients.
 *
 * HOWEVER: if our hook installs AFTER SDL2 already called connect (timing
 * race), we miss the display. To handle this, we ALSO hook
 * wl_display_dispatch_queue_pending (called every frame by SDL2's event
 * loop) — when we see a dispatch call, we lazily init the sidecar using
 * the display from that call.
 */

static struct wl_display *(*orig_wl_display_connect)(const char *name) = NULL;
static struct wl_display *(*orig_wl_display_connect_to_fd)(int fd) = NULL;
/* orig_wl_display_dispatch_queue_pending forward-declared above, before
 * idk_wayland_input_sidecar_dispatch() */

static struct wl_display *hook_wl_display_connect(const char *name) {
    struct wl_display *display = orig_wl_display_connect
        ? orig_wl_display_connect(name)
        : NULL;
    if (display) {
        WLOG("wl_display_connect(\"%s\") → %p — initializing sidecar",
             name ? name : "(default)", (void *)display);
        sidecar_init(display);
    }
    return display;
}

static struct wl_display *hook_wl_display_connect_to_fd(int fd) {
    struct wl_display *display = orig_wl_display_connect_to_fd
        ? orig_wl_display_connect_to_fd(fd)
        : NULL;
    if (display) {
        WLOG("wl_display_connect_to_fd(%d) → %p — initializing sidecar",
             fd, (void *)display);
        sidecar_init(display);
    }
    return display;
}

static int hook_wl_display_dispatch_queue_pending(struct wl_display *display,
                                                   struct wl_event_queue *queue) {
    /* Re-entry guard — sidecar_init calls roundtrip_queue which calls
     * dispatch_queue_pending internally. Without this guard, the hook
     * re-enters sidecar_init → infinite recursion → crash. */
    static int s_in_sidecar_init = 0;

    /* When inside sidecar_init, call the ORIGINAL function via bounce stub
     * — NOT real_ (dlsym), which would hit the inline trampoline and
     * re-enter our hook. orig_ runs the saved prologue and jumps past
     * the trampoline, safely calling the real function body. */
    if (s_in_sidecar_init) {
        if (orig_wl_display_dispatch_queue_pending)
            return orig_wl_display_dispatch_queue_pending(display, queue);
        if (real_wl_display_dispatch_queue_pending)
            return real_wl_display_dispatch_queue_pending(display, queue);
        return -1;
    }

    /* Lazy sidecar init — if connect hooks missed (timing race), catch
     * the display here. This fires every frame, so we'll init within
     * 1 frame of the game starting its event loop. */
    if (!g_sidecar_initialized && display) {
        s_in_sidecar_init = 1;
        WLOG("wl_display_dispatch_queue_pending: display=%p — lazy sidecar init",
             (void *)display);
        sidecar_init(display);
        s_in_sidecar_init = 0;
        WLOG("sidecar_init returned, g_sidecar_initialized=%d", g_sidecar_initialized);
    }

    /* Scan event queue's proxy_list for game's keyboards AND pointers.
     * Game may have registered before our hook installed. */
    scan_and_intercept_input_proxies(display);

    /* DO NOT dispatch sidecar queue here — calling dispatch_queue_pending
     * on our sidecar queue from inside the game's dispatch hook causes
     * reentrancy issues (prepare_read conflicts, crashes). The sidecar
     * queue is dispatched from idk_wayland_input_sidecar_dispatch() which
     * is called from the EGL swap hook (separate call site, safe). */

    /* Call the ORIGINAL dispatch for the game's display+queue.
     * IMPORTANT: use orig_ (syringe bounce stub), NOT real_ (dlsym).
     * syringe installs an inline trampoline at the start of the real
     * function. If we call real_ directly, we hit the trampoline →
     * re-enter our hook → infinite loop / deadlock. orig_ is the bounce
     * stub that runs the saved prologue then jumps past the trampoline. */
    if (orig_wl_display_dispatch_queue_pending) {
        return orig_wl_display_dispatch_queue_pending(display, queue);
    }
    /* Fallback: if orig is NULL (hook not fully set up), use real_ */
    if (real_wl_display_dispatch_queue_pending) {
        return real_wl_display_dispatch_queue_pending(display, queue);
    }
    return -1;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int idk_wayland_input_init(void) {
    if (g_hook_installed) return 0;

    if (resolve_wayland_symbols() != 0) {
        return -1;  /* not a wayland client, or libwayland not loaded */
    }

    /* xkbcommon is optional — gracefully degrade if unavailable */
    resolve_xkbcommon_symbols();

    configure_hotkey();

    if (init_input_socket() != 0) {
        WLOG("input socket init failed — events will be dropped (no webview)");
        /* Continue anyway — hooks still useful for hotkey detection */
    }

    /* Install the hook via syringe — this catches all callers in the
     * process (including libraries like SDL, GLFW, Qt that link
     * libwayland-client normally). */
    int n = syringe_hook_install("wl_proxy_add_listener",
                                  (void *)hook_wl_proxy_add_listener,
                                  (void **)&orig_wl_proxy_add_listener);
    if (n <= 0) {
        WERR("syringe_hook_install(wl_proxy_add_listener) failed: n=%d", n);
        return -1;
    }

    /* Also hook dispatcher for diagnostic logging (pass-through for now) */
    int n2 = syringe_hook_install("wl_proxy_add_dispatcher",
                                   (void *)hook_wl_proxy_add_dispatcher,
                                   (void **)&orig_wl_proxy_add_dispatcher);
    if (n2 <= 0) {
        WLOG("wl_proxy_add_dispatcher hook not installed (n=%d) — OK for SDL/GLFW games", n2);
    }

    /* Hook wl_display_connect + wl_display_connect_to_fd to capture the
     * display for sidecar init. SDL2 uses connect_to_fd (it opens the
     * socket itself); other clients use connect. Hook both to be safe. */
    int n3 = syringe_hook_install("wl_display_connect",
                                   (void *)hook_wl_display_connect,
                                   (void **)&orig_wl_display_connect);
    if (n3 <= 0) {
        WLOG("wl_display_connect hook not installed (n=%d)", n3);
    }

    int n4 = syringe_hook_install("wl_display_connect_to_fd",
                                   (void *)hook_wl_display_connect_to_fd,
                                   (void **)&orig_wl_display_connect_to_fd);
    if (n4 <= 0) {
        WLOG("wl_display_connect_to_fd hook not installed (n=%d) — "
             "SDL2 Wayland clients may not get sidecar via connect path", n4);
    }

    /* Hook wl_display_dispatch_queue_pending — this is called EVERY FRAME
     * by SDL2's event loop. It's our RELIABLE fallback for two things:
     * 1. Lazy sidecar init if connect hooks missed (timing race)
     * 2. Dispatching our sidecar's private event queue (process keyboard
     *    events for hotkey detection)
     * This is the most important hook — it guarantees we catch the display
     * and process input regardless of when our hook installed. */
    int n5 = syringe_hook_install("wl_display_dispatch_queue_pending",
                                   (void *)hook_wl_display_dispatch_queue_pending,
                                   (void **)&orig_wl_display_dispatch_queue_pending);
    if (n5 <= 0) {
        WLOG("wl_display_dispatch_queue_pending hook not installed (n=%d) — "
             "sidecar lazy init + dispatch will not work", n5);
    }

    g_hook_installed = 1;
    WLOG("hooks installed: add_listener=%d add_dispatcher=%d "
         "display_connect=%d display_connect_to_fd=%d dispatch_queue_pending=%d",
         n, n2, n3, n4, n5);
    return 0;
}

void idk_wayland_input_shutdown(void) {
    if (!g_hook_installed) return;

    /* During process exit, don't remove hooks or destroy wayland objects —
     * the game's wayland display is being torn down concurrently and
     * syringe_hook_remove can race with active calls. Just close our
     * input socket and let the OS reclaim the rest. */
    g_hook_installed = 0;
    g_sidecar_initialized = 0;
    g_sidecar_display = NULL;
    g_sidecar_keyboard = NULL;
    g_sidecar_seat = NULL;
    g_sidecar_queue = NULL;

    if (g_accept_thread_started) {
        if (g_input_listen_fd >= 0) {
            shutdown(g_input_listen_fd, SHUT_RDWR);
            close(g_input_listen_fd);
            g_input_listen_fd = -1;
        }
        pthread_join(g_accept_thread, NULL);
        g_accept_thread_started = 0;
    }

    pthread_mutex_lock(&g_client_fd_lock);
    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
    }
    pthread_mutex_unlock(&g_client_fd_lock);

    if (g_xkb_state && fn_xkb_state_unref) {
        fn_xkb_state_unref(g_xkb_state);
        g_xkb_state = NULL;
    }
    if (g_xkb_keymap && fn_xkb_keymap_unref) {
        fn_xkb_keymap_unref(g_xkb_keymap);
        g_xkb_keymap = NULL;
    }
    if (g_xkb_ctx && fn_xkb_context_unref) {
        fn_xkb_context_unref(g_xkb_ctx);
        g_xkb_ctx = NULL;
    }

    if (g_xkb_handle) {
        dlclose(g_xkb_handle);
        g_xkb_handle = NULL;
    }
    if (g_wl_handle) {
        dlclose(g_wl_handle);
        g_wl_handle = NULL;
    }
}
