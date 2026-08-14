#include "hook/wayland_internal.h"

#include "../internal.h"

/* Cursor shape interface definitions */
static const struct wl_message g_device_requests[] = {
    {"destroy", "", NULL},
    {"set_shape", "uu", NULL},
};
IDK_INTERNAL const struct wl_interface g_wp_cursor_shape_device_v1_interface = {
    "wp_cursor_shape_device_v1", 2, 2, g_device_requests, 0, NULL,
};

static const struct wl_interface *g_manager_get_pointer_types[] = {
    &g_wp_cursor_shape_device_v1_interface,
    NULL,
};
static const struct wl_message g_manager_requests[] = {
    {"destroy", "", NULL},
    {"get_pointer", "no", g_manager_get_pointer_types},
};
IDK_INTERNAL const struct wl_interface g_wp_cursor_shape_manager_v1_interface = {
    "wp_cursor_shape_manager_v1", 2, 2, g_manager_requests, 0, NULL,
};
