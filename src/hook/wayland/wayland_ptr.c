#include "hook/wayland_internal.h"

/* Cursor/pointer globals */
int32_t g_cursor_x = 0;
int32_t g_cursor_y = 0;
uint32_t g_last_enter_serial = 0;
uint32_t g_last_pointer_serial = 0;
struct wl_surface *g_last_enter_surface = NULL;
struct wl_proxy *g_game_pointer_proxy = NULL;
int g_pointer_in_surface = 0;
int g_game_cursor_hidden = 0;
int g_pre_capture_cursor_hidden = 0;

/* Forward declarations */
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

/* Listener wrapper vtable (non-static - checked by proxy scan) */
const struct wl_pointer_listener g_ptr_wrapper = {
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

/* Pointer callbacks */

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
        if (g_cursor_shape_device)
            my_wp_cursor_shape_device_set_shape(
                g_cursor_shape_device, serial,
                WP_CURSOR_SHAPE_DEFAULT);
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
        idk_input_event_t ev = { 0 };
        ev.type   = IDK_INPUT_MOTION;
        ev.time   = time;
        ev.u.motion.x = g_cursor_x;
        ev.u.motion.y = g_cursor_y;
        ev.u.motion._p1 = 0;
        ev.mods   = (uint16_t)g_mods;
        ev.flags  = IDK_INPUT_FLAG_CAPTURE;
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
        idk_input_event_t ev = { 0 };
        ev.type   = IDK_INPUT_BUTTON;
        ev.time   = time;
        ev.u.btn.x = g_cursor_x;
        ev.u.btn.y = g_cursor_y;
        ev.u.btn.button = button;
        ev.flags  = state ? IDK_INPUT_FLAG_PRESS : 0;
        ev.flags |= IDK_INPUT_FLAG_CAPTURE;
        ev.mods   = (uint16_t)g_mods;
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
        idk_input_event_t ev = { 0 };
        ev.type   = IDK_INPUT_AXIS;
        ev.time   = time;
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            ev.u.axis.dy = WL_FIXED_TO_INT(value);
        else
            ev.u.axis.dx = WL_FIXED_TO_INT(value);
        ev.u.axis._p1 = 0;
        ev.mods   = (uint16_t)g_mods;
        ev.flags  = IDK_INPUT_FLAG_CAPTURE;
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
