#include "hook/syringe_hook.h"
#include "hook/wayland_internal.h"
#include <stdatomic.h>

/* Capture state */
/* These are read from the compositor render path (egl_hook.c /
 * glx_hook.c / vulkan_layer.c) and written from the input hook path
 * (wayland_kb.c / sidecar.c / x11_kb.c). In practice both run on the
 * game's main thread, but C11 _Atomic is the correct tool for
 * cross-thread visibility — 'volatile' is only a compiler barrier
 * and is UB under the C11 memory model. */
_Atomic int g_captured = 0;
_Atomic int g_hotkey_pressed = 0;
uint32_t g_hotkey_keysym = 0;
uint32_t g_hotkey_scancode = 0;
uint32_t g_hotkey_mods = 0;
uint32_t g_mods = 0;

static int g_hook_installed = 0;

/* Resolved wayland function pointers */
wl_proxy_add_listener_fn    real_wl_proxy_add_listener    = NULL;
wl_proxy_add_dispatcher_fn  real_wl_proxy_add_dispatcher  = NULL;
wl_proxy_get_class_fn       real_wl_proxy_get_class       = NULL;
wl_proxy_get_listener_fn    real_wl_proxy_get_listener    = NULL;

wl_display_create_queue_fn          real_wl_display_create_queue = NULL;
wl_proxy_create_wrapper_fn          real_wl_proxy_create_wrapper = NULL;
wl_proxy_wrapper_destroy_fn         real_wl_proxy_wrapper_destroy = NULL;
wl_proxy_set_queue_fn               real_wl_proxy_set_queue = NULL;
wl_display_roundtrip_queue_fn       real_wl_display_roundtrip_queue = NULL;
wl_display_dispatch_queue_pending_fn real_wl_display_dispatch_queue_pending = NULL;
wl_event_queue_destroy_fn           real_wl_event_queue_destroy = NULL;
wl_proxy_get_version_fn             real_wl_proxy_get_version = NULL;
wl_proxy_destroy_fn                 real_wl_proxy_destroy = NULL;
wl_proxy_marshal_constructor_versioned_fn real_wl_proxy_marshal_constructor_versioned = NULL;
wl_proxy_marshal_flags_fn           real_wl_proxy_marshal_flags = NULL;
wl_proxy_marshal_array_flags_fn     real_wl_proxy_marshal_array_flags = NULL;

const struct wl_interface *g_wl_seat_interface = NULL;
const struct wl_interface *g_wl_keyboard_interface = NULL;
const struct wl_interface *g_wl_registry_interface = NULL;
const struct wl_interface *g_wl_pointer_interface = NULL;

void *g_wl_handle = NULL;

/* Resolve wayland symbols */
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

/* Resolve xkbcommon symbols */
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

/* Hotkey table */
struct hotkey_name_to_scancode {
    const char *name;
    uint32_t scancode;
    uint32_t keysym;
};

static const struct hotkey_name_to_scancode HOTKEY_TABLE[] = {
    { "Tab",           IDK_KEY_TAB,          IDK_XKB_KEY_Tab },
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

void configure_hotkey(void) {
    const char *env = getenv("IDK_HOTKEY_CAPTURE");
    if (!env || !env[0]) env = "Shift+Tab";

    uint32_t mods = 0;
    const char *keyname = env;

    const char *plus = strchr(env, '+');
    if (plus && plus > env) {
        size_t n = (size_t)(plus - env);
        char mod[32];
        if (n < sizeof(mod)) {
            memcpy(mod, env, n);
            mod[n] = '\0';
            if (strcasecmp(mod, "Shift") == 0)      mods = IDK_MOD_SHIFT;
            else if (strcasecmp(mod, "Ctrl") == 0)   mods = IDK_MOD_CTRL;
            else if (strcasecmp(mod, "Alt") == 0)    mods = IDK_MOD_ALT;
            else if (strcasecmp(mod, "Super") == 0)  mods = IDK_MOD_SUPER;
            keyname = plus + 1;
        }
    }
    g_hotkey_mods = mods;

    if (fn_xkb_keysym_from_name) {
        uint32_t ks = fn_xkb_keysym_from_name(keyname, IDK_XKB_KEYSYM_NO_FLAGS);
        if (ks != 0) {
            g_hotkey_keysym = ks;
            for (size_t i = 0; i < sizeof(HOTKEY_TABLE) / sizeof(HOTKEY_TABLE[0]); i++) {
                if (strcmp(keyname, HOTKEY_TABLE[i].name) == 0) {
                    g_hotkey_scancode = HOTKEY_TABLE[i].scancode;
                    break;
                }
            }
            WLOG("hotkey: %s (mods=0x%x keysym=0x%x scancode=%u)",
                 env, g_hotkey_mods, g_hotkey_keysym, g_hotkey_scancode);
            return;
        }
    }

    for (size_t i = 0; i < sizeof(HOTKEY_TABLE) / sizeof(HOTKEY_TABLE[0]); i++) {
        if (strcmp(keyname, HOTKEY_TABLE[i].name) == 0) {
            g_hotkey_keysym = HOTKEY_TABLE[i].keysym;
            g_hotkey_scancode = HOTKEY_TABLE[i].scancode;
            WLOG("hotkey (from table): %s (mods=0x%x keysym=0x%x scancode=%u)",
                 env, g_hotkey_mods, g_hotkey_keysym, g_hotkey_scancode);
            return;
        }
    }

    g_hotkey_keysym = IDK_XKB_KEY_Tab;
    g_hotkey_scancode = IDK_KEY_TAB;
    g_hotkey_mods = IDK_MOD_SHIFT;
    WLOG("unknown hotkey '%s', falling back to Shift+Tab", env);
}

/* Set capture */

void idk_wayland_input_set_capture(int enable) {
    int new_state = enable ? 1 : 0;
    if (new_state == g_captured) return;

    g_captured = new_state;

    if (idk_vk_layer_is_active()) {
        WLOG("set_capture(%s): skipping cursor ops (Vulkan layer mode)",
             new_state ? "ON" : "OFF");
        send_capture_state((uint32_t)new_state);
        if (new_state)
            send_repeat_info();
        return;
    }

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

    if (new_state)
        send_repeat_info();
}

int idk_wayland_input_is_captured(void) {
    return g_captured;
}

/* Init */

int idk_wayland_input_init(void) {
    if (g_hook_installed) return 0;

    if (resolve_wayland_symbols() != 0)
        return -1;

    resolve_xkbcommon_symbols();
    configure_hotkey();

    if (init_input_socket() != 0)
        WLOG("input socket init failed — events will be dropped (no webview)");

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
    if (n2 <= 0)
        WLOG("wl_proxy_add_dispatcher hook not installed (n=%d)", n2);

    int n3 = syringe_hook_install("wl_display_connect",
                                   (void *)hook_wl_display_connect,
                                   (void **)&orig_wl_display_connect);
    if (n3 <= 0)
        WLOG("wl_display_connect hook not installed (n=%d)", n3);

    int n4 = syringe_hook_install("wl_display_connect_to_fd",
                                   (void *)hook_wl_display_connect_to_fd,
                                   (void **)&orig_wl_display_connect_to_fd);
    if (n4 <= 0)
        WLOG("wl_display_connect_to_fd hook not installed (n=%d)", n4);

    int n5 = syringe_hook_install("wl_display_dispatch_queue_pending",
                                   (void *)hook_wl_display_dispatch_queue_pending,
                                   (void **)&orig_wl_display_dispatch_queue_pending);
    if (n5 <= 0)
        WLOG("wl_display_dispatch_queue_pending hook not installed (n=%d)", n5);

    int n6 = syringe_hook_install("wl_proxy_marshal_array_flags",
                                   (void *)hook_wl_proxy_marshal_array_flags,
                                   (void **)&orig_wl_proxy_marshal_array_flags);
    if (n6 <= 0)
        WLOG("wl_proxy_marshal_array_flags hook not installed (n=%d)", n6);

    int n7 = syringe_hook_install("wl_proxy_destroy",
                                   (void *)hook_wl_proxy_destroy,
                                   (void **)&orig_wl_proxy_destroy);
    if (n7 <= 0)
        WLOG("wl_proxy_destroy hook not installed (n=%d)", n7);

    g_hook_installed = 1;
    WLOG("hooks installed: add_listener=%d add_dispatcher=%d "
         "display_connect=%d display_connect_to_fd=%d dispatch_queue_pending=%d "
         "marshal_array=%d proxy_destroy=%d",
         n, n2, n3, n4, n5, n6, n7);
    return 0;
}

/* Shutdown */

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

    teardown_input_socket();
    teardown_xkb();

    if (g_wl_handle) {
        dlclose(g_wl_handle);
        g_wl_handle = NULL;
    }
}
