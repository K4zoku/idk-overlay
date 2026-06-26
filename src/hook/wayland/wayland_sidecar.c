#include "hook/wayland_internal.h"

/* ── Sidecar globals ─────────────────────────────────────────────────── */
struct wl_display *g_sidecar_display = NULL;
struct wl_event_queue *g_sidecar_queue = NULL;
struct wl_seat *g_sidecar_seat = NULL;
struct wl_keyboard *g_sidecar_keyboard = NULL;
struct wl_pointer *g_sidecar_pointer = NULL;
struct wl_proxy *g_sidecar_cursor_shape_manager = NULL;
int g_sidecar_initialized = 0;
int g_sidecar_ready = 0;

struct wl_proxy *g_cursor_shape_device = NULL;
struct wl_proxy *g_shape_device_pointer_proxy = NULL;

uint32_t g_sidecar_pointer_enter_serial = 0;
void *g_sidecar_surface = NULL;
wl_fixed_t g_sidecar_sx = 0;
wl_fixed_t g_sidecar_sy = 0;

/* ── Cursor shape interface definitions ──────────────────────────────── */
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

/* ── my_wl_* protocol wrappers ───────────────────────────────────────── */

static struct wl_registry *my_wl_display_get_registry(struct wl_display *display) {
    if (!real_wl_proxy_marshal_constructor_versioned || !g_wl_registry_interface)
        return NULL;
    return (struct wl_registry *)real_wl_proxy_marshal_constructor_versioned(
        (struct wl_proxy *)display, WL_DISPLAY_GET_REGISTRY,
        g_wl_registry_interface, real_wl_proxy_get_version((struct wl_proxy *)display), NULL);
}

static void *my_wl_registry_bind(struct wl_registry *registry, uint32_t name,
                                  const struct wl_interface *interface, uint32_t version) {
    if (!real_wl_proxy_marshal_constructor_versioned || !g_wl_registry_interface)
        return NULL;
    return real_wl_proxy_marshal_constructor_versioned(
        (struct wl_proxy *)registry, WL_REGISTRY_BIND,
        interface, version, name, interface->name, version, NULL);
}

static struct wl_keyboard *my_wl_seat_get_keyboard(struct wl_seat *seat) {
    if (!real_wl_proxy_marshal_constructor_versioned || !g_wl_keyboard_interface)
        return NULL;
    return (struct wl_keyboard *)real_wl_proxy_marshal_constructor_versioned(
        (struct wl_proxy *)seat, WL_SEAT_GET_KEYBOARD,
        g_wl_keyboard_interface, real_wl_proxy_get_version((struct wl_proxy *)seat), NULL);
}

static struct wl_pointer *my_wl_seat_get_pointer(struct wl_seat *seat) {
    if (!real_wl_proxy_marshal_constructor_versioned || !g_wl_pointer_interface)
        return NULL;
    return (struct wl_pointer *)real_wl_proxy_marshal_constructor_versioned(
        (struct wl_proxy *)seat, WL_SEAT_GET_POINTER,
        g_wl_pointer_interface, real_wl_proxy_get_version((struct wl_proxy *)seat), NULL);
}

void my_wp_cursor_shape_device_set_shape(struct wl_proxy *device,
                                          uint32_t serial, uint32_t shape) {
    if (!real_wl_proxy_marshal_flags) return;
    real_wl_proxy_marshal_flags(device, WP_CURSOR_SHAPE_DEVICE_SET_SHAPE,
        NULL, 2, 0, serial, shape);
}

void my_wl_pointer_set_cursor(struct wl_proxy *p, uint32_t serial,
                               struct wl_surface *surface, int32_t hx, int32_t hy) {
    if (!real_wl_proxy_marshal_flags) return;
    real_wl_proxy_marshal_flags(p, WL_POINTER_SET_CURSOR,
        NULL, real_wl_proxy_get_version(p), 0,
        serial, surface, hx, hy);
}

/* ── Sidecar pointer listener ────────────────────────────────────────── */

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

/* ── Sidecar keyboard listener ───────────────────────────────────────── */

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
    if (g_xkb_state && fn_xkb_state_key_get_one_sym)
        keysym = fn_xkb_state_key_get_one_sym(g_xkb_state, key + 8);

    if (g_xkb_state && fn_xkb_state_update_key)
        fn_xkb_state_update_key(g_xkb_state, key + 8,
                                state ? IDK_XKB_KEY_DOWN : IDK_XKB_KEY_UP);

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

/* ── Sidecar seat listener ───────────────────────────────────────────── */

static void sidecar_seat_capabilities(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_sidecar_pointer) {
        g_sidecar_pointer = my_wl_seat_get_pointer(seat);
        if (g_sidecar_pointer) {
            void *old_data = NULL;
            direct_overwrite_implementation(
                (struct wl_proxy *)g_sidecar_pointer,
                (void *)&g_sidecar_ptr_listener, NULL, &old_data);
            WLOG("sidecar: wl_pointer bound + listener installed (old_impl=%p)", old_data);
        }
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_sidecar_keyboard) {
        g_sidecar_keyboard = my_wl_seat_get_keyboard(seat);
        if (g_sidecar_keyboard) {
            void *old_data = NULL;
            direct_overwrite_implementation(
                (struct wl_proxy *)g_sidecar_keyboard,
                (void *)&g_sidecar_kb_listener, NULL, &old_data);
            WLOG("sidecar: wl_keyboard bound + listener installed (old_impl=%p)", old_data);
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

/* ── Sidecar registry listener ───────────────────────────────────────── */

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
        if (g_sidecar_seat && real_wl_proxy_add_listener)
            real_wl_proxy_add_listener((struct wl_proxy *)g_sidecar_seat,
                                       (void (**)(void))&g_sidecar_seat_listener, NULL);
    } else if (strcmp(iface, "wp_cursor_shape_manager_v1") == 0 && !g_sidecar_cursor_shape_manager) {
        if (idk_vk_layer_is_active()) {
            WLOG("sidecar: skipping wp_cursor_shape_manager_v1 (Vulkan layer mode)");
            return;
        }
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

/* ── Sidecar init ────────────────────────────────────────────────────── */

int sidecar_init(struct wl_display *display) {
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

    if (real_wl_display_roundtrip_queue) {
        real_wl_display_roundtrip_queue(display, g_sidecar_queue);
        g_sidecar_ready = 1;
    }

    WLOG("sidecar: initialized (seat=%p keyboard=%p ready=%d)",
         (void *)g_sidecar_seat, (void *)g_sidecar_keyboard, g_sidecar_ready);
    return 0;
}

void sidecar_ensure_cursor_shape_device(struct wl_pointer *p) {
    if (!g_sidecar_cursor_shape_manager) return;
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

void idk_wayland_input_sidecar_dispatch(void) {
    if (!g_sidecar_ready || !g_sidecar_display || !g_sidecar_queue) return;
    if (!orig_wl_display_dispatch_queue_pending) return;
    orig_wl_display_dispatch_queue_pending(g_sidecar_display, g_sidecar_queue);
}

/* ── Display connect hooks ───────────────────────────────────────────── */

struct wl_display *(*orig_wl_display_connect)(const char *name) = NULL;
struct wl_display *(*orig_wl_display_connect_to_fd)(int fd) = NULL;

struct wl_display *hook_wl_display_connect(const char *name) {
    struct wl_display *display = orig_wl_display_connect
        ? orig_wl_display_connect(name)
        : NULL;
    if (display) {
        WLOG("wl_display_connect(\"%s\") → %p", name ? name : "(default)", (void *)display);
        sidecar_init(display);
    }
    return display;
}

struct wl_display *hook_wl_display_connect_to_fd(int fd) {
    struct wl_display *display = orig_wl_display_connect_to_fd
        ? orig_wl_display_connect_to_fd(fd)
        : NULL;
    if (display) {
        WLOG("wl_display_connect_to_fd(%d) → %p", fd, (void *)display);
        sidecar_init(display);
    }
    return display;
}

int (*orig_wl_display_dispatch_queue_pending)(struct wl_display *, struct wl_event_queue *) = NULL;

int hook_wl_display_dispatch_queue_pending(struct wl_display *display,
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
    if (orig_wl_display_dispatch_queue_pending)
        ret = orig_wl_display_dispatch_queue_pending(display, queue);
    else if (real_wl_display_dispatch_queue_pending)
        ret = real_wl_display_dispatch_queue_pending(display, queue);
    else
        ret = -1;
    g_in_dispatch = 0;
    return ret;
}

/* ── Marshal array flags hook (track cursor state) ───────────────────── */

struct wl_proxy *(*orig_wl_proxy_marshal_array_flags)(
    struct wl_proxy *, uint32_t, const struct wl_interface *,
    uint32_t, uint32_t, union wl_argument *) = NULL;

struct wl_proxy *hook_wl_proxy_marshal_array_flags(
    struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface,
    uint32_t version, uint32_t flags, union wl_argument *args) {
    if (proxy && proxy == g_game_pointer_proxy && opcode == WL_POINTER_SET_CURSOR
        && args && !g_captured)
        g_game_cursor_hidden = (args[1].o == NULL);
    if (orig_wl_proxy_marshal_array_flags)
        return orig_wl_proxy_marshal_array_flags(proxy, opcode, interface,
                                                  version, flags, args);
    if (real_wl_proxy_marshal_array_flags)
        return real_wl_proxy_marshal_array_flags(proxy, opcode, interface,
                                                  version, flags, args);
    return NULL;
}

/* ── Proxy destroy hook (cleanup cursor shape device) ────────────────── */

void (*orig_wl_proxy_destroy)(struct wl_proxy *) = NULL;

void hook_wl_proxy_destroy(struct wl_proxy *proxy) {
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

    if (proxy == (struct wl_proxy *)g_sidecar_pointer)
        g_sidecar_pointer = NULL;

    if (orig_wl_proxy_destroy)
        orig_wl_proxy_destroy(proxy);
    else if (real_wl_proxy_destroy)
        real_wl_proxy_destroy(proxy);
}
