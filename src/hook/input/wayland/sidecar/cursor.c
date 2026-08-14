#include "hook/wayland_internal.h"

#include "../internal.h"

/* Sidecar pointer listener */

static void sidecar_ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t sx,
                              wl_fixed_t sy) {
  (void)d;
  (void)p;
  g_sidecar_pointer_enter_serial = serial;
  g_sidecar_surface = (void *)s;
  g_sidecar_sx = sx;
  g_sidecar_sy = sy;
  if (g_game_pointer_proxy && !g_pointer_in_surface) {
    void **data_ptr = (void **)((char *)g_game_pointer_proxy + WL_PROXY_DATA_OFFSET);
    struct ptr_state *st = (struct ptr_state *)*data_ptr;
    if (st && st->game && st->game->enter) {
      g_pointer_in_surface = 1;
      g_last_enter_serial = serial;
      g_last_pointer_serial = serial;
      st->game->enter(st->game_data, (struct wl_pointer *)g_game_pointer_proxy, serial, s, sx, sy);
    }
  }
}

static void sidecar_ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
  (void)d;
  (void)p;
  (void)serial;
  (void)s;
}
static void sidecar_ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
  (void)d;
  (void)p;
  (void)time;
  (void)sx;
  (void)sy;
}
static void sidecar_ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button,
                               uint32_t state) {
  (void)d;
  (void)p;
  (void)time;
  (void)button;
  (void)state;
  g_sidecar_pointer_enter_serial = serial;
}
static void sidecar_ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value) {
  (void)d;
  (void)p;
  (void)time;
  (void)axis;
  (void)value;
}
static void sidecar_ptr_frame(void *d, struct wl_pointer *p) {
  (void)d;
  (void)p;
}
static void sidecar_ptr_axis_source(void *d, struct wl_pointer *p, uint32_t src) {
  (void)d;
  (void)p;
  (void)src;
}
static void sidecar_ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis) {
  (void)d;
  (void)p;
  (void)time;
  (void)axis;
}
static void sidecar_ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t disc) {
  (void)d;
  (void)p;
  (void)axis;
  (void)disc;
}

IDK_INTERNAL const struct wl_pointer_listener g_sidecar_ptr_listener = {
    .enter = sidecar_ptr_enter,
    .leave = sidecar_ptr_leave,
    .motion = sidecar_ptr_motion,
    .button = sidecar_ptr_button,
    .axis = sidecar_ptr_axis,
    .frame = sidecar_ptr_frame,
    .axis_source = sidecar_ptr_axis_source,
    .axis_stop = sidecar_ptr_axis_stop,
    .axis_discrete = sidecar_ptr_axis_discrete,
};

/* my_wl_* protocol wrappers */

void my_wp_cursor_shape_device_set_shape(struct wl_proxy *device, uint32_t serial, uint32_t shape) {
  if (!real_wl_proxy_marshal_flags)
    return;
  real_wl_proxy_marshal_flags(device, WP_CURSOR_SHAPE_DEVICE_SET_SHAPE, NULL, 2, 0, serial, shape);
}

void my_wl_pointer_set_cursor(struct wl_proxy *p, uint32_t serial, struct wl_surface *surface, int32_t hx, int32_t hy) {
  if (!real_wl_proxy_marshal_flags)
    return;
  real_wl_proxy_marshal_flags(p, WL_POINTER_SET_CURSOR, NULL, real_wl_proxy_get_version(p), 0, serial, surface, hx, hy);
}

void sidecar_ensure_cursor_shape_device(struct wl_pointer *p) {
  if (!g_sidecar_cursor_shape_manager)
    return;
  if (g_cursor_shape_device && g_shape_device_pointer_proxy == (struct wl_proxy *)p)
    return;

  if (g_cursor_shape_device) {
    real_wl_proxy_marshal_flags(g_cursor_shape_device, 0, NULL, 2, WL_MARSHAL_FLAG_DESTROY);
    g_cursor_shape_device = NULL;
    g_shape_device_pointer_proxy = NULL;
  }
  g_cursor_shape_device = real_wl_proxy_marshal_constructor_versioned(
      g_sidecar_cursor_shape_manager, 1, &g_wp_cursor_shape_device_v1_interface, 2, NULL, p);
  if (!g_cursor_shape_device) {
    WERR("cursor: get_pointer failed");
    return;
  }
  g_shape_device_pointer_proxy = (struct wl_proxy *)p;
}
