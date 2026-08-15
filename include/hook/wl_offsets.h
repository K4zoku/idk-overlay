#ifndef IDK_WL_OFFSETS_H
#define IDK_WL_OFFSETS_H

#include "hook/wayland_input_types.h"

/* Vendored Wayland ABI layouts (struct wl_interface, wire arguments) */
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

struct wl_list {
  struct wl_list *prev;
  struct wl_list *next;
};

/* Wrapper state structs */
struct ptr_state {
  const struct wl_pointer_listener *game;
  void *game_data;
};

struct kb_state {
  const struct wl_keyboard_listener *game;
  void *game_data;
};

/* Private offsets into struct wl_proxy / struct wl_event_queue */
#define WL_PROXY_IMPL_OFFSET 8
#define WL_PROXY_DATA_OFFSET 48
#define WL_EVENT_QUEUE_PROXY_LIST_OFFSET 16
#define WL_PROXY_QUEUE_LINK_OFFSET 80
#define MAX_INTERCEPTED 16

/* Wayland opcode constants */
#define WL_DISPLAY_GET_REGISTRY 1
#define WL_REGISTRY_BIND 0
#define WL_SEAT_GET_POINTER 0
#define WL_SEAT_GET_KEYBOARD 1
#define WP_CURSOR_SHAPE_DEVICE_SET_SHAPE 1
#define WL_POINTER_SET_CURSOR 0
#define WL_COMPOSITOR_CREATE_SURFACE 0
#define WL_SHM_CREATE_POOL 0
#define WL_SHM_POOL_CREATE_BUFFER 0
#define WL_SHM_POOL_DESTROY 1
#define WL_SURFACE_DESTROY 0
#define WL_SURFACE_ATTACH 1
#define WL_SURFACE_DAMAGE 2
#define WL_SURFACE_COMMIT 6
#define WL_SURFACE_SET_BUFFER_SCALE 8
#define WL_BUFFER_DESTROY 0
#define WP_CURSOR_SHAPE_DEFAULT 1
#define WP_CURSOR_SHAPE_CROSSHAIR 8
#define WL_MARSHAL_FLAG_DESTROY 1
#define WL_SHM_FORMAT_ARGB8888 0

/* Wayland protocol constants */
#define WL_KEYBOARD_KEY_STATE_RELEASED 0u
#define WL_KEYBOARD_KEY_STATE_PRESSED 1u
#define WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 1u
#define WL_POINTER_AXIS_VERTICAL_SCROLL 0u
#define WL_POINTER_AXIS_HORIZONTAL_SCROLL 1u
#define WL_SEAT_CAPABILITY_KEYBOARD 2
#define WL_SEAT_CAPABILITY_POINTER 1

#endif /* IDK_WL_OFFSETS_H */
