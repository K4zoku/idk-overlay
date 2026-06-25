/* wayland_input.c */

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

struct wl_compositor;
struct wl_shm;
struct wl_buffer;
struct wl_shm_pool;

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

/* wl_interface struct layout (from wayland-util.h) */
struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const void *methods;  /* const struct wl_message * */
    int event_count;
    const void *events;   /* const struct wl_message * */
};

/* wl_argument union (from wayland-util.h) */
union wl_argument {
    int32_t i;
    uint32_t u;
    wl_fixed_t f;
    const char *s;
    void *o;       /* struct wl_object * (we use void* to avoid defining wl_object) */
    int32_t h;     /* fd */
    void *a;       /* struct wl_array * */
    uint32_t n;    /* new_id */
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
typedef struct wl_proxy *(*wl_proxy_marshal_array_flags_fn)(
    struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface,
    uint32_t version, uint32_t flags,
    union wl_argument *args);

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
static wl_proxy_marshal_array_flags_fn     real_wl_proxy_marshal_array_flags = NULL;


static const struct wl_interface *g_wl_seat_interface = NULL;
static const struct wl_interface *g_wl_keyboard_interface = NULL;
static const struct wl_interface *g_wl_registry_interface = NULL;
static const struct wl_interface *g_wl_pointer_interface = NULL;

/* Crafted manually — not in libwayland-client */
static const struct wl_message g_device_requests[] = {
    { "destroy", "", NULL },
    { "set_shape", "uu", NULL },
};
static const struct wl_interface g_wp_cursor_shape_device_v1_interface = {
    "wp_cursor_shape_device_v1", 2, 2, g_device_requests, 0, NULL,
};


static const struct wl_interface *g_manager_get_pointer_types[] = {
    &g_wp_cursor_shape_device_v1_interface,
    NULL,
};
static const struct wl_message g_manager_requests[] = {
    { "destroy", "", NULL },
    { "get_pointer", "no", g_manager_get_pointer_types },
};
static const struct wl_interface g_wp_cursor_shape_manager_v1_interface = {
    "wp_cursor_shape_manager_v1", 2, 2, g_manager_requests, 0, NULL,
};

#define WL_DISPLAY_GET_REGISTRY 1
#define WL_REGISTRY_BIND 0
#define WL_SEAT_GET_POINTER 0
#define WL_SEAT_GET_KEYBOARD 1
#define WP_CURSOR_SHAPE_DEVICE_SET_SHAPE 1
#define WL_POINTER_SET_CURSOR 0

#define WP_CURSOR_SHAPE_DEFAULT   1
#define WP_CURSOR_SHAPE_CROSSHAIR 8

#define WL_MARSHAL_FLAG_DESTROY 1

/* Manual implementation of static-inline wayland functions */
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

static struct wl_pointer *my_wl_seat_get_pointer(struct wl_seat *seat) {
    if (!real_wl_proxy_marshal_flags || !g_wl_pointer_interface)
        return NULL;
    return (struct wl_pointer *)real_wl_proxy_marshal_flags(
        (struct wl_proxy *)seat, WL_SEAT_GET_POINTER,
        g_wl_pointer_interface,
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

/* wp_cursor_shape_device_v1 marshal helpers */
static void my_wp_cursor_shape_device_set_shape(struct wl_proxy *device,
    uint32_t serial, uint32_t shape) {
    if (!real_wl_proxy_marshal_flags) return;
    real_wl_proxy_marshal_flags(device, WP_CURSOR_SHAPE_DEVICE_SET_SHAPE,
        NULL, 2, 0, serial, shape);
}

/* Hide/show cursor — restore pre-capture state */
static void my_wl_pointer_set_cursor(struct wl_proxy *p, uint32_t serial,
    void *surface, int32_t hx, int32_t hy) {
    if (!real_wl_proxy_marshal_flags) return;
    real_wl_proxy_marshal_flags(p, WL_POINTER_SET_CURSOR,
        NULL, real_wl_proxy_get_version(p), 0,
        serial, surface, hx, hy);
}


static struct wl_display *g_sidecar_display = NULL;
static struct wl_event_queue *g_sidecar_queue = NULL;
static struct wl_seat *g_sidecar_seat = NULL;
static struct wl_keyboard *g_sidecar_keyboard = NULL;
static struct wl_pointer *g_sidecar_pointer = NULL;
static struct wl_proxy *g_sidecar_cursor_shape_manager = NULL;
static int g_sidecar_initialized = 0;

/* Cursor shape device */
static struct wl_proxy *g_cursor_shape_device = NULL;
static struct wl_proxy *g_shape_device_pointer_proxy = NULL;

/* Fresh serial/surface from compositor for set_shape */
static uint32_t g_sidecar_pointer_enter_serial = 0;
static void *g_sidecar_surface = NULL;
static wl_fixed_t g_sidecar_sx = 0;
static wl_fixed_t g_sidecar_sy = 0;

/* Game's cursor hidden state — tracked via marshal hook */
static int g_game_cursor_hidden = 0;
static int g_pre_capture_cursor_hidden = 0;


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


static void *g_wl_handle  = NULL;
static void *g_xkb_handle = NULL;
static struct xkb_context *g_xkb_ctx = NULL;

static int g_hook_installed = 0;
static int g_input_listen_fd = -1;
static int g_client_fd = -1;
static pthread_t g_accept_thread;
static int g_accept_thread_started = 0;
static pthread_mutex_t g_client_fd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Capture state — toggled by hotkey */
static volatile int g_captured = 0;
static int g_hotkey_pressed = 0;
static uint32_t g_hotkey_keysym = 0;
static uint32_t g_hotkey_scancode = 0;

/* Keyboard repeat info (from wl_keyboard.repeat_info) */
static int32_t g_repeat_rate = 25;   /* characters per second (default) */
static int32_t g_repeat_delay = 500; /* milliseconds before first repeat (default) */


static struct xkb_keymap *g_xkb_keymap = NULL;
static struct xkb_state  *g_xkb_state  = NULL;


static uint32_t g_mod_idx_ctrl  = UINT32_MAX;  /* invalid until keymap arrives */
static uint32_t g_mod_idx_shift = UINT32_MAX;
static uint32_t g_mod_idx_alt   = UINT32_MAX;
static uint32_t g_mod_idx_super = UINT32_MAX;


static uint32_t g_mods = 0;


static int32_t g_cursor_x = 0;
static int32_t g_cursor_y = 0;


static uint32_t g_last_enter_serial = 0;
static uint32_t g_last_pointer_serial = 0;  /* latest serial from any pointer event */
static struct wl_surface *g_last_enter_surface = NULL;
static struct wl_proxy *g_game_pointer_proxy = NULL;
static int g_pointer_in_surface = 0;

#define WLOG(fmt, ...) IDK_LOG("wl-input", fmt "\n", ##__VA_ARGS__)
#define WERR(fmt, ...) IDK_ERR("wl-input", fmt "\n", ##__VA_ARGS__)

#define WL_INT_TO_FIXED(i) ((wl_fixed_t)((i) * 256))

static int resolve_wayland_symbols(void) {
    if (g_wl_handle) return 0;

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
    real_wl_proxy_marshal_array_flags = (wl_proxy_marshal_array_flags_fn)
        dlsym(g_wl_handle, "wl_proxy_marshal_array_flags");

    g_wl_seat_interface = (const struct wl_interface *)
        dlsym(g_wl_handle, "wl_seat_interface");
    g_wl_keyboard_interface = (const struct wl_interface *)
        dlsym(g_wl_handle, "wl_keyboard_interface");
    g_wl_registry_interface = (const struct wl_interface *)
        dlsym(g_wl_handle, "wl_registry_interface");
    g_wl_pointer_interface = (const struct wl_interface *)
        dlsym(g_wl_handle, "wl_pointer_interface");

    WLOG("libwayland-client resolved: add_listener=%p get_class=%p sidecar=%s",
         (void *)real_wl_proxy_add_listener, (void *)real_wl_proxy_get_class,
         (real_wl_display_create_queue && g_wl_seat_interface) ? "OK" : "MISSING");
    return 0;
}

static int resolve_xkbcommon_symbols(void) {
    if (g_xkb_handle) return 0;

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

static void *accept_thread_main(void *arg) {
    (void)arg;
    int listen_fd = g_input_listen_fd;
    while (1) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        pthread_mutex_lock(&g_client_fd_lock);
        if (g_client_fd >= 0) {
            close(g_client_fd);
        }
        g_client_fd = fd;
        pthread_mutex_unlock(&g_client_fd_lock);
        WLOG("webview connected to input socket (fd=%d)", fd);
    }
    return NULL;
}

static int init_input_socket(void) {
    char path[128];
    const char *base = getenv("IDK_SOCKET");
    if (base && base[0]) {
        snprintf(path, sizeof(path), "%.107s-input", base);
    } else {
        snprintf(path, sizeof(path), "/tmp/idk-overlay-%d-input", (int)getpid());
    }

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

static void send_event_to_webview(const idk_ipc_input_event_t *ev) {
    pthread_mutex_lock(&g_client_fd_lock);
    int fd = g_client_fd;
    pthread_mutex_unlock(&g_client_fd_lock);
    if (fd < 0) {
        WLOG("send_event_to_webview: DROPPED (no webview connected, fd=-1)");
        return;
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

/* Send keyboard repeat info to webview so it can implement client-side
 * repeat when captured (game's SDL3 repeat timer never starts because
 * we swallow key press events). */
static void send_repeat_info(void) {
    idk_ipc_input_event_t ev = { 0 };
    ev.type = IDK_INPUT_REPEAT;
    ev.x    = g_repeat_rate;   /* rate in characters per second */
    ev.y    = g_repeat_delay;  /* delay in milliseconds before first repeat */
    send_event_to_webview(&ev);
}

struct ptr_state {
    const struct wl_pointer_listener *game;
    void *game_data;
};

struct kb_state {
    const struct wl_keyboard_listener *game;
    void *game_data;
};

static void sidecar_ensure_cursor_shape_device(struct wl_pointer *p);



int idk_wayland_input_is_captured(void) {
    return g_captured;
}

void idk_wayland_input_set_capture(int enable) {
    int new_state = enable ? 1 : 0;
    if (new_state == g_captured) return;

    g_captured = new_state;

    if (g_game_pointer_proxy) {
        sidecar_ensure_cursor_shape_device(
            (struct wl_pointer *)g_game_pointer_proxy);

        uint32_t serial = g_last_enter_serial ? g_last_enter_serial
                       : g_sidecar_pointer_enter_serial ? g_sidecar_pointer_enter_serial
                       : g_last_pointer_serial;

        if (new_state) {
            g_pre_capture_cursor_hidden = g_game_cursor_hidden;
            if (g_cursor_shape_device && serial) {
                WLOG("set_capture(ON): serial=%u shape=crosshair device=%p "
                     "(enter=%u sidecar_enter=%u ptr=%u pre_hidden=%d)",
                     serial, (void *)g_cursor_shape_device,
                     g_last_enter_serial, g_sidecar_pointer_enter_serial,
                     g_last_pointer_serial, g_pre_capture_cursor_hidden);
                my_wp_cursor_shape_device_set_shape(
                    g_cursor_shape_device, serial, WP_CURSOR_SHAPE_DEFAULT);
            }
        } else {
            if (g_pointer_in_surface && g_game_pointer_proxy) {
                void **data_ptr = (void **)((char *)g_game_pointer_proxy + 48);
                struct ptr_state *st = (struct ptr_state *)*data_ptr;
                if (st && st->game && st->game->motion) {
                    WLOG("set_capture(OFF): re-sync motion to game (%d,%d)",
                         g_cursor_x, g_cursor_y);
                    st->game->motion(st->game_data,
                        (struct wl_pointer *)g_game_pointer_proxy, 0,
                        WL_INT_TO_FIXED(g_cursor_x), WL_INT_TO_FIXED(g_cursor_y));
                }
            }
            if (g_cursor_shape_device && serial) {
                if (g_pre_capture_cursor_hidden) {
                    WLOG("set_capture(OFF): serial=%u → hide cursor "
                         "(pre-capture was hidden)", serial);
                    my_wl_pointer_set_cursor(
                        g_game_pointer_proxy, serial, NULL, 0, 0);
                } else {
                    WLOG("set_capture(OFF): serial=%u shape=default device=%p",
                         serial, (void *)g_cursor_shape_device);
                    my_wp_cursor_shape_device_set_shape(
                        g_cursor_shape_device, serial, WP_CURSOR_SHAPE_DEFAULT);
                }
            }
        }
    } else if (!new_state) {
        WLOG("set_capture(OFF): no game pointer proxy — cursor unchanged");
    }

    WLOG("input capture %s", new_state ? "ENABLED" : "DISABLED");
    send_capture_state((uint32_t)new_state);

    /* Send repeat info when capture goes ON so webview can start
     * its client-side repeat timer with the correct rate/delay. */
    if (new_state)
        send_repeat_info();
}

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



static void wptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                       struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
    struct ptr_state *st = (struct ptr_state *)d;
    g_cursor_x = WL_FIXED_TO_INT(sx);
    g_cursor_y = WL_FIXED_TO_INT(sy);

    g_last_enter_serial = serial;
    g_last_pointer_serial = serial;
    g_last_enter_surface = s;
    g_game_pointer_proxy = (struct wl_proxy *)p;
    g_pointer_in_surface = 1;

    sidecar_ensure_cursor_shape_device(p);

    if (g_captured) {
        if (st->game && st->game->enter)
            st->game->enter(st->game_data, p, serial, s, sx, sy);
        if (g_cursor_shape_device) {
            my_wp_cursor_shape_device_set_shape(
                g_cursor_shape_device, serial,
                WP_CURSOR_SHAPE_DEFAULT);
        }
        return;
    }

    if (st->game && st->game->enter)
        st->game->enter(st->game_data, p, serial, s, sx, sy);
}

static void wptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                       struct wl_surface *s) {
    struct ptr_state *st = (struct ptr_state *)d;
    g_pointer_in_surface = 0;
    if (st->game && st->game->leave)
        st->game->leave(st->game_data, p, serial, s);
}

static void wptr_motion(void *d, struct wl_pointer *p, uint32_t time,
                        wl_fixed_t sx, wl_fixed_t sy) {
    struct ptr_state *st = (struct ptr_state *)d;
    g_cursor_x = WL_FIXED_TO_INT(sx);
    g_cursor_y = WL_FIXED_TO_INT(sy);

    if (g_captured) {
        if (g_cursor_shape_device && g_last_enter_serial)
            my_wp_cursor_shape_device_set_shape(
                g_cursor_shape_device, g_last_enter_serial,
                WP_CURSOR_SHAPE_DEFAULT);
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
    g_last_pointer_serial = serial;
    if (!g_pointer_in_surface) {
        g_pointer_in_surface = 1;
        g_game_pointer_proxy = (struct wl_proxy *)p;
    }
    if (g_captured) {
        if (g_cursor_shape_device && g_last_enter_serial)
            my_wp_cursor_shape_device_set_shape(
                g_cursor_shape_device, g_last_enter_serial,
                WP_CURSOR_SHAPE_DEFAULT);
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
        if (g_cursor_shape_device && g_last_enter_serial)
            my_wp_cursor_shape_device_set_shape(
                g_cursor_shape_device, g_last_enter_serial,
                WP_CURSOR_SHAPE_DEFAULT);
        idk_ipc_input_event_t ev = { 0 };
        ev.type   = IDK_INPUT_AXIS;
        ev.time   = time;
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
    if (g_captured) return;
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



static void wkb_keymap(void *d, struct wl_keyboard *kb, uint32_t fmt, int32_t fd, uint32_t sz) {
    struct kb_state *st = (struct kb_state *)d;

    int dup_fd = -1;
    if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && g_xkb_handle && g_xkb_ctx) {
        dup_fd = dup(fd);
        if (dup_fd < 0) {
            WLOG("keymap dup() failed: %s — xkb disabled", strerror(errno));
        }
    }

    if (st->game && st->game->keymap) {
        st->game->keymap(st->game_data, kb, fmt, fd, sz);
    }

    if (dup_fd >= 0 && fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        void *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, dup_fd, 0);
        if (map == MAP_FAILED) {
            WLOG("keymap mmap failed: %s", strerror(errno));
        } else {
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
    if (g_xkb_state && fn_xkb_state_key_get_one_sym) {
        return fn_xkb_state_key_get_one_sym(g_xkb_state, key + 8);
    }
    return 0;  /* unknown */
}

static int is_hotkey(uint32_t key, uint32_t keysym) {
    if (g_hotkey_keysym && keysym == g_hotkey_keysym) return 1;
    if (g_hotkey_scancode && key == g_hotkey_scancode) return 1;
    return 0;
}

static void wkb_key(void *d, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state) {
    struct kb_state *st = (struct kb_state *)d;

    if (key == 0 && state == 0) return;

    uint32_t keysym = decode_keysym(key);

    /* Update xkb state for keysym resolution + modifier tracking */
    if (g_xkb_state && fn_xkb_state_update_key) {
        fn_xkb_state_update_key(g_xkb_state, key + 8,
                                state ? IDK_XKB_KEY_DOWN : IDK_XKB_KEY_UP);
    }

    if (is_hotkey(key, keysym)) {
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
        WLOG("wkb_key: keycode=%u state=%u", key, state);
        send_event_to_webview(&ev);
        return;
    }
    if (st->game && st->game->key)
        st->game->key(st->game_data, kb, serial, time, key, state);
}

static void update_mod_bitmask(void) {

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

    if (g_xkb_state && fn_xkb_state_update_mask) {
        fn_xkb_state_update_mask(g_xkb_state, dep, lat, lck, 0, 0, grp);
        update_mod_bitmask();
    }

    if (g_captured) return;
    if (st->game && st->game->modifiers)
        st->game->modifiers(st->game_data, kb, serial, dep, lat, lck, grp);
}

static void wkb_repeat_info(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay) {
    struct kb_state *st = (struct kb_state *)d;

    /* Store repeat info even when captured — webview needs it for
     * client-side key repeat (game's SDL3 timer never starts because
     * we swallow key press events). */
    g_repeat_rate = rate;
    g_repeat_delay = delay;
    WLOG("repeat_info: rate=%d cps delay=%d ms", rate, delay);
    send_repeat_info();

    /* Forward to game's listener (always, not just when !captured) */
    if (st->game && st->game->repeat_info)
        st->game->repeat_info(st->game_data, kb, rate, delay);
}

/* Scan wl_event_queue.proxy_list for game keyboards/pointers */

/* Forward declarations — defined later in the file */
struct wl_list { struct wl_list *prev; struct wl_list *next; };
static void *direct_overwrite_implementation(struct wl_proxy *proxy,
                                              void *new_impl, void *new_data,
                                              void **old_data_out);

#define WL_EVENT_QUEUE_PROXY_LIST_OFFSET 16
#define WL_PROXY_QUEUE_LINK_OFFSET 80

#define MAX_INTERCEPTED 16
static struct wl_proxy *g_intercepted_proxies[MAX_INTERCEPTED];
static int g_intercepted_count = 0;
static pthread_mutex_t g_scan_mutex = PTHREAD_MUTEX_INITIALIZER;

static __thread int g_in_dispatch = 0;

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

    if (g_in_dispatch) {
        return;
    }

    pthread_mutex_lock(&g_scan_mutex);

    struct wl_proxy *display_proxy = (struct wl_proxy *)display;
    struct wl_event_queue *queue = *(struct wl_event_queue **)((char *)display_proxy + 32);
    if (!queue) {
        pthread_mutex_unlock(&g_scan_mutex);
        return;
    }

    struct wl_list *head = (struct wl_list *)((char *)queue + WL_EVENT_QUEUE_PROXY_LIST_OFFSET);
    struct wl_list *item = head->next;

    int found = 0;
    unsigned int iterations = 0;
    while (item != head && iterations < (MAX_INTERCEPTED * 4) && found < MAX_INTERCEPTED) {
        iterations++;
        struct wl_proxy *proxy = (struct wl_proxy *)((char *)item - WL_PROXY_QUEUE_LINK_OFFSET);
        struct wl_list *next = item->next;
        if (next == NULL || next == head || next == item) {
            // WERR("proxy list corrupted at iteration %u, aborting scan", iterations);
            break;
        }
        item = next;

        const char *cls = real_wl_proxy_get_class(proxy);
        if (!cls) continue;

        void **impl_ptr = (void **)((char *)proxy + 8);
        if (*impl_ptr == (void *)&g_kb_wrapper ||
            *impl_ptr == (void *)&g_ptr_wrapper) continue;
        if (is_already_intercepted(proxy)) continue;

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
            g_game_pointer_proxy = proxy;
            WLOG("scan: intercepted game pointer: proxy=%p game=%p data=%p",
                 (void *)proxy, (void *)st->game, st->game_data);
            if (g_sidecar_pointer_enter_serial && st->game && st->game->enter) {
                WLOG("scan: synthetic enter for cursor state init "
                     "(serial=%u surface=%p)", g_sidecar_pointer_enter_serial,
                     g_sidecar_surface);
                g_last_enter_serial = g_sidecar_pointer_enter_serial;
                g_last_pointer_serial = g_sidecar_pointer_enter_serial;
                g_pointer_in_surface = 1;
                st->game->enter(st->game_data,
                    (struct wl_pointer *)proxy,
                    g_sidecar_pointer_enter_serial,
                    (struct wl_surface *)g_sidecar_surface,
                    g_sidecar_sx, g_sidecar_sy);

            }
            found++;
        }
    }

    pthread_mutex_unlock(&g_scan_mutex);
}



static int (*orig_wl_proxy_add_listener)(struct wl_proxy *, void (**)(void), void *) = NULL;

/* Direct overwrite — bypass "already has listener" check */
static void *direct_overwrite_implementation(struct wl_proxy *proxy,
                                              void *new_impl, void *new_data,
                                              void **old_data_out) {
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
        if (orig_wl_proxy_add_listener)
            return orig_wl_proxy_add_listener(proxy, impl, data);
        return -1;
    }

    const char *cls = real_wl_proxy_get_class(proxy);
    if (!cls) {
        return orig_wl_proxy_add_listener
            ? orig_wl_proxy_add_listener(proxy, impl, data)
            : real_wl_proxy_add_listener(proxy, impl, data);
    }

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

    if (strcmp(cls, "wl_pointer") == 0) {
        struct ptr_state *st = (struct ptr_state *)calloc(1, sizeof(*st));
        if (!st) goto passthrough;
        st->game      = (const struct wl_pointer_listener *)impl;
        st->game_data = data;
        void *old_data = NULL;
        void *old_impl = direct_overwrite_implementation(
            proxy, (void *)&g_ptr_wrapper, st, &old_data);
        if (old_impl) {
            st->game = (const struct wl_pointer_listener *)old_impl;
            st->game_data = old_data;
        }
        g_game_pointer_proxy = proxy;
        WLOG("intercepted wl_pointer: game=%p data=%p proxy=%p (direct overwrite, old_impl=%p)",
             (void *)st->game, st->game_data, (void *)proxy, old_impl);
        return 0;
    }

    if (strcmp(cls, "wl_keyboard") == 0) {
        struct kb_state *st = (struct kb_state *)calloc(1, sizeof(*st));
        if (!st) goto passthrough;
        st->game      = (const struct wl_keyboard_listener *)impl;
        st->game_data = data;
        void *old_data = NULL;
        void *old_impl = direct_overwrite_implementation(
            proxy, (void *)&g_kb_wrapper, st, &old_data);
        if (old_impl) {
            st->game = (const struct wl_keyboard_listener *)old_impl;
            st->game_data = old_data;
        }
        WLOG("intercepted wl_keyboard: game=%p data=%p proxy=%p (direct overwrite, old_impl=%p)",
             (void *)st->game, st->game_data, (void *)proxy, old_impl);
        return 0;
    }

passthrough:
    return orig_wl_proxy_add_listener
        ? orig_wl_proxy_add_listener(proxy, impl, data)
        : real_wl_proxy_add_listener(proxy, impl, data);
}

/* Dispatcher hook — pass-through for now */

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

/* Linux input-event-codes.h scancodes (fallback when xkb unavailable) */
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
    if (!env || !env[0]) env = "F8";

    if (fn_xkb_keysym_from_name) {
        uint32_t ks = fn_xkb_keysym_from_name(env, IDK_XKB_KEYSYM_NO_FLAGS);
        if (ks != 0) {
            g_hotkey_keysym = ks;
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

    for (size_t i = 0; i < sizeof(HOTKEY_TABLE) / sizeof(HOTKEY_TABLE[0]); i++) {
        if (strcmp(env, HOTKEY_TABLE[i].name) == 0) {
            g_hotkey_keysym = HOTKEY_TABLE[i].keysym;
            g_hotkey_scancode = HOTKEY_TABLE[i].scancode;
            WLOG("hotkey (from table): %s (keysym=0x%x scancode=%u)",
                 env, g_hotkey_keysym, g_hotkey_scancode);
            return;
        }
    }

    g_hotkey_keysym = IDK_XKB_KEY_F8;
    g_hotkey_scancode = IDK_KEY_F8;
    WLOG("unknown hotkey '%s', falling back to F8", env);
}

/* Sidecar (MangoHud-style) — duplicate key events for hotkey detection */

#define WL_SEAT_CAPABILITY_KEYBOARD 2
#define WL_SEAT_CAPABILITY_POINTER  1

static void sidecar_ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                              struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d; (void)p;
    g_sidecar_pointer_enter_serial = serial;
    g_sidecar_surface = (void *)s;
    g_sidecar_sx = sx;
    g_sidecar_sy = sy;
    if (g_game_pointer_proxy && !g_pointer_in_surface) {
        void **data_ptr = (void **)((char *)g_game_pointer_proxy + 48);
        struct ptr_state *st = (struct ptr_state *)*data_ptr;
        if (st && st->game && st->game->enter) {
            g_pointer_in_surface = 1;
            g_last_enter_serial = serial;
            g_last_pointer_serial = serial;
            st->game->enter(st->game_data,
                (struct wl_pointer *)g_game_pointer_proxy,
                serial, s, sx, sy);
        }
    }
}
static void sidecar_ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                              struct wl_surface *s) { (void)d; (void)p; (void)serial; (void)s; }
static void sidecar_ptr_motion(void *d, struct wl_pointer *p, uint32_t time,
                               wl_fixed_t sx, wl_fixed_t sy) { (void)d; (void)p; (void)time; (void)sx; (void)sy; }
static void sidecar_ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                               uint32_t time, uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)time; (void)button; (void)state;
    g_sidecar_pointer_enter_serial = serial;
}
static void sidecar_ptr_axis(void *d, struct wl_pointer *p, uint32_t time,
                             uint32_t axis, wl_fixed_t value) { (void)d; (void)p; (void)time; (void)axis; (void)value; }
static void sidecar_ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void sidecar_ptr_axis_source(void *d, struct wl_pointer *p, uint32_t src) { (void)d; (void)p; (void)src; }
static void sidecar_ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis) { (void)d; (void)p; (void)time; (void)axis; }
static void sidecar_ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t disc) { (void)d; (void)p; (void)axis; (void)disc; }

static const struct wl_pointer_listener g_sidecar_ptr_listener = {
    .enter          = sidecar_ptr_enter,
    .leave          = sidecar_ptr_leave,
    .motion         = sidecar_ptr_motion,
    .button         = sidecar_ptr_button,
    .axis           = sidecar_ptr_axis,
    .frame          = sidecar_ptr_frame,
    .axis_source    = sidecar_ptr_axis_source,
    .axis_stop      = sidecar_ptr_axis_stop,
    .axis_discrete  = sidecar_ptr_axis_discrete,
};

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

    if (key == 0 && state == 0) return;

    uint32_t keysym = 0;
    if (g_xkb_state && fn_xkb_state_key_get_one_sym) {
        keysym = fn_xkb_state_key_get_one_sym(g_xkb_state, key + 8);
    }

    if (g_xkb_state && fn_xkb_state_update_key) {
        fn_xkb_state_update_key(g_xkb_state, key + 8,
                                state ? IDK_XKB_KEY_DOWN : IDK_XKB_KEY_UP);
    }

    if (is_hotkey(key, keysym)) {
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
    (void)d; (void)kb;
    /* Sidecar gets its own repeat_info from the compositor.
     * Store and forward to webview (same as game's wkb_repeat_info). */
    g_repeat_rate = rate;
    g_repeat_delay = delay;
    WLOG("sidecar repeat_info: rate=%d cps delay=%d ms", rate, delay);
    send_repeat_info();
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
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_sidecar_pointer) {
        g_sidecar_pointer = my_wl_seat_get_pointer(seat);
        if (g_sidecar_pointer) {
            void *old_data = NULL;
            void *old_impl = direct_overwrite_implementation(
                (struct wl_proxy *)g_sidecar_pointer,
                (void *)&g_sidecar_ptr_listener, NULL, &old_data);
            WLOG("sidecar: wl_pointer bound + listener installed (old_impl=%p)", old_impl);
        }
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_sidecar_keyboard) {
        g_sidecar_keyboard = my_wl_seat_get_keyboard(seat);
        if (g_sidecar_keyboard) {
            void *old_data = NULL;
            void *old_impl = direct_overwrite_implementation(
                (struct wl_proxy *)g_sidecar_keyboard,
                (void *)&g_sidecar_kb_listener, NULL, &old_data);
            WLOG("sidecar: wl_keyboard bound + listener installed (old_impl=%p)", old_impl);
        }
    }
}

static void sidecar_seat_name(void *d, struct wl_seat *seat, const char *name) {
    (void)d; (void)seat; (void)name;

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
        }
    } else if (strcmp(iface, "wp_cursor_shape_manager_v1") == 0 && !g_sidecar_cursor_shape_manager) {
        uint32_t bind_ver = ver < 2 ? ver : 2;
        g_sidecar_cursor_shape_manager = (struct wl_proxy *)my_wl_registry_bind(
            reg, name, &g_wp_cursor_shape_manager_v1_interface, bind_ver);
        WLOG("sidecar: wp_cursor_shape_manager_v1 bound → %p (ver=%u)",
             (void *)g_sidecar_cursor_shape_manager, bind_ver);
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

    static int s_sidecar_failed = 0;
    if (s_sidecar_failed) return -1;

    if (!real_wl_display_create_queue || !real_wl_proxy_create_wrapper ||
        !real_wl_proxy_wrapper_destroy || !real_wl_proxy_set_queue ||
        !real_wl_display_roundtrip_queue || !real_wl_display_dispatch_queue_pending ||
        !real_wl_proxy_get_version || !real_wl_proxy_destroy ||
        !real_wl_proxy_marshal_constructor_versioned || !real_wl_proxy_marshal_flags ||
        !g_wl_seat_interface || !g_wl_keyboard_interface || !g_wl_registry_interface) {
        s_sidecar_failed = 1;
        WLOG("sidecar: missing wayland symbols");
        return -1;
    }

    g_sidecar_display = display;

    g_sidecar_queue = real_wl_display_create_queue(display);
    if (!g_sidecar_queue) {
        s_sidecar_failed = 1;
        return -1;
    }

    struct wl_proxy *display_wrapper = real_wl_proxy_create_wrapper((struct wl_proxy *)display);
    if (!display_wrapper) {
        s_sidecar_failed = 1;
        return -1;
    }
    real_wl_proxy_set_queue(display_wrapper, g_sidecar_queue);

    struct wl_registry *registry = my_wl_display_get_registry((struct wl_display *)display_wrapper);
    real_wl_proxy_wrapper_destroy(display_wrapper);

    if (!registry) {
        s_sidecar_failed = 1;
        return -1;
    }

    real_wl_proxy_add_listener((struct wl_proxy *)registry,
                               (void (**)(void))&g_sidecar_registry_listener, NULL);

    g_sidecar_initialized = 1;
    WLOG("sidecar: initialized (seat=%p keyboard=%p)",
         (void *)g_sidecar_seat, (void *)g_sidecar_keyboard);
    return 0;
}

/* Cursor shape device lifecycle */
static void sidecar_ensure_cursor_shape_device(struct wl_pointer *p) {
    if (!g_sidecar_cursor_shape_manager) {
        return;
    }
    if (g_cursor_shape_device &&
        g_shape_device_pointer_proxy == (struct wl_proxy *)p)
        return;

    if (g_cursor_shape_device) {
        real_wl_proxy_marshal_flags(g_cursor_shape_device, 0,
            NULL, 2, WL_MARSHAL_FLAG_DESTROY);
        g_cursor_shape_device = NULL;
        g_shape_device_pointer_proxy = NULL;
    }
    g_cursor_shape_device = real_wl_proxy_marshal_constructor_versioned(
        g_sidecar_cursor_shape_manager, 1,
        &g_wp_cursor_shape_device_v1_interface, 2,
        NULL, p);
    if (!g_cursor_shape_device) {
        WERR("cursor: get_pointer failed");
        return;
    }
    g_shape_device_pointer_proxy = (struct wl_proxy *)p;
}

static int (*orig_wl_display_dispatch_queue_pending)(struct wl_display *, struct wl_event_queue *) = NULL;

void idk_wayland_input_sidecar_dispatch(void) {
    if (!g_sidecar_initialized || !g_sidecar_display || !g_sidecar_queue) return;
    if (!orig_wl_display_dispatch_queue_pending) return;
    orig_wl_display_dispatch_queue_pending(g_sidecar_display, g_sidecar_queue);
}

/* Display connect hooks — init sidecar lazily */

static struct wl_display *(*orig_wl_display_connect)(const char *name) = NULL;
static struct wl_display *(*orig_wl_display_connect_to_fd)(int fd) = NULL;


static struct wl_display *hook_wl_display_connect(const char *name) {
    struct wl_display *display = orig_wl_display_connect
        ? orig_wl_display_connect(name)
        : NULL;
    if (display) {
        WLOG("wl_display_connect(\"%s\") → %p", name ? name : "(default)", (void *)display);
        sidecar_init(display);
    }
    return display;
}

static struct wl_display *hook_wl_display_connect_to_fd(int fd) {
    struct wl_display *display = orig_wl_display_connect_to_fd
        ? orig_wl_display_connect_to_fd(fd)
        : NULL;
    if (display) {
        WLOG("wl_display_connect_to_fd(%d) → %p", fd, (void *)display);
        sidecar_init(display);
    }
    return display;
}

static int hook_wl_display_dispatch_queue_pending(struct wl_display *display,
                                                    struct wl_event_queue *queue) {
    static int s_in_sidecar_init = 0;

    if (s_in_sidecar_init) {
        if (orig_wl_display_dispatch_queue_pending)
            return orig_wl_display_dispatch_queue_pending(display, queue);
        if (real_wl_display_dispatch_queue_pending)
            return real_wl_display_dispatch_queue_pending(display, queue);
        return -1;
    }

    if (!g_sidecar_initialized && display) {
        s_in_sidecar_init = 1;
        sidecar_init(display);
        s_in_sidecar_init = 0;
    }

    scan_and_intercept_input_proxies(display);

    g_in_dispatch = 1;
    int ret;
    if (orig_wl_display_dispatch_queue_pending) {
        ret = orig_wl_display_dispatch_queue_pending(display, queue);
    } else if (real_wl_display_dispatch_queue_pending) {
        ret = real_wl_display_dispatch_queue_pending(display, queue);
    } else {
        ret = -1;
    }
    g_in_dispatch = 0;
    return ret;
}

/* Track game cursor state via set_cursor interception */

static struct wl_proxy *(*orig_wl_proxy_marshal_array_flags)(
    struct wl_proxy *, uint32_t, const struct wl_interface *,
    uint32_t, uint32_t, union wl_argument *) = NULL;

static struct wl_proxy *hook_wl_proxy_marshal_array_flags(
    struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface,
    uint32_t version, uint32_t flags, union wl_argument *args) {
    if (proxy && proxy == g_game_pointer_proxy && opcode == WL_POINTER_SET_CURSOR
        && args && !g_captured) {
        g_game_cursor_hidden = (args[1].o == NULL);
    }
    if (orig_wl_proxy_marshal_array_flags)
        return orig_wl_proxy_marshal_array_flags(proxy, opcode, interface,
                                                  version, flags, args);
    if (real_wl_proxy_marshal_array_flags)
        return real_wl_proxy_marshal_array_flags(proxy, opcode, interface,
                                                  version, flags, args);
    return NULL;
}

/* Clean up stale cursor shape device on pointer recreate */

static void (*orig_wl_proxy_destroy)(struct wl_proxy *) = NULL;

static void hook_wl_proxy_destroy(struct wl_proxy *proxy) {
    if (proxy && proxy == g_game_pointer_proxy) {
        WLOG("wl_proxy_destroy: game pointer %p — destroying cursor shape device",
             (void *)proxy);
        if (g_cursor_shape_device) {
            real_wl_proxy_marshal_flags(g_cursor_shape_device, 0,
                NULL, 2, WL_MARSHAL_FLAG_DESTROY);
            g_cursor_shape_device = NULL;
            g_shape_device_pointer_proxy = NULL;
        }
        g_game_pointer_proxy = NULL;
        g_pointer_in_surface = 0;
    }

    if (proxy == (struct wl_proxy *)g_sidecar_pointer) {
        g_sidecar_pointer = NULL;
    }

    if (orig_wl_proxy_destroy)
        orig_wl_proxy_destroy(proxy);
    else if (real_wl_proxy_destroy)
        real_wl_proxy_destroy(proxy);
}



int idk_wayland_input_init(void) {
    if (g_hook_installed) return 0;

    if (resolve_wayland_symbols() != 0) {
        return -1;
    }

    resolve_xkbcommon_symbols();

    configure_hotkey();

    if (init_input_socket() != 0) {
        WLOG("input socket init failed — events will be dropped (no webview)");
    }


    int n = syringe_hook_install("wl_proxy_add_listener",
                                  (void *)hook_wl_proxy_add_listener,
                                  (void **)&orig_wl_proxy_add_listener);
    if (n <= 0) {
        WERR("syringe_hook_install(wl_proxy_add_listener) failed: n=%d", n);
        return -1;
    }


    int n2 = syringe_hook_install("wl_proxy_add_dispatcher",
                                   (void *)hook_wl_proxy_add_dispatcher,
                                   (void **)&orig_wl_proxy_add_dispatcher);
    if (n2 <= 0) {
        WLOG("wl_proxy_add_dispatcher hook not installed (n=%d)", n2);
    }


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
        WLOG("wl_display_connect_to_fd hook not installed (n=%d)", n4);
    }


    int n5 = syringe_hook_install("wl_display_dispatch_queue_pending",
                                   (void *)hook_wl_display_dispatch_queue_pending,
                                   (void **)&orig_wl_display_dispatch_queue_pending);
    if (n5 <= 0) {
        WLOG("wl_display_dispatch_queue_pending hook not installed (n=%d)", n5);
    }


    int n6 = syringe_hook_install("wl_proxy_marshal_array_flags",
                                   (void *)hook_wl_proxy_marshal_array_flags,
                                   (void **)&orig_wl_proxy_marshal_array_flags);
    if (n6 <= 0) {
        WLOG("wl_proxy_marshal_array_flags hook not installed (n=%d)", n6);
    }


    int n7 = syringe_hook_install("wl_proxy_destroy",
                                   (void *)hook_wl_proxy_destroy,
                                   (void **)&orig_wl_proxy_destroy);
    if (n7 <= 0) {
        WLOG("wl_proxy_destroy hook not installed (n=%d)", n7);
    }

    g_hook_installed = 1;
    WLOG("hooks installed: add_listener=%d add_dispatcher=%d "
         "display_connect=%d display_connect_to_fd=%d dispatch_queue_pending=%d "
         "marshal_array=%d proxy_destroy=%d",
         n, n2, n3, n4, n5, n6, n7);
    return 0;
}

void idk_wayland_input_shutdown(void) {
    if (!g_hook_installed) return;

    g_hook_installed = 0;
    g_sidecar_initialized = 0;
    g_sidecar_display = NULL;
    g_sidecar_keyboard = NULL;
    g_sidecar_seat = NULL;
    g_sidecar_cursor_shape_manager = NULL;
    g_sidecar_queue = NULL;

    g_cursor_shape_device = NULL;
    g_shape_device_pointer_proxy = NULL;

    orig_wl_proxy_destroy = NULL;

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
