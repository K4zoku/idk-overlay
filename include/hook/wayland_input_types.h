/*
 * wayland_input_types.h — Minimal vendored Wayland protocol types
 *
 * Only includes what idk-overlay's input hook needs:
 *   - wl_fixed_t (int32_t)
 *   - wl_pointer_listener / wl_keyboard_listener struct layouts
 *   - State/format enum constants
 *   - Opaque struct forward declarations
 *
 * This avoids a build-time dependency on libwayland-dev. The real
 * libwayland-client.so.0 is loaded at runtime via dlopen.
 *
 * Struct layouts here MUST match the public wayland-client-protocol.h
 * ABI exactly. They are stable since wayland 1.0 (pointer listener)
 * and 1.2 (keyboard listener).
 *
 * Source: wayland-protocols / wayland-client-protocol.h (verified
 * against libwayland-client 1.23.1 on this system).
 */
#ifndef IDK_WAYLAND_INPUT_TYPES_H
#define IDK_WAYLAND_INPUT_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque wayland object types (forward declarations) */

struct wl_proxy;
struct wl_pointer;
struct wl_keyboard;
struct wl_touch;
struct wl_surface;
struct wl_array;
struct wl_display;
struct wl_seat;
struct wl_registry;
struct wl_event_queue;

/* wl_message — from wayland-util.h, needed for manual protocol interface
 * construction (wp_cursor_shape_manager_v1, wp_cursor_shape_device_v1). */
struct wl_interface;
struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;
};

/* Fixed-point (24.8 signed) */

typedef int32_t wl_fixed_t;

#define WL_FIXED_TO_INT(f)     ((int32_t)(f) / 256)
#define WL_FIXED_TO_DOUBLE(f)  ((double)(f) / 256.0)
#define WL_FIXED_FROM_INT(i)   ((wl_fixed_t)((i) * 256))
#define WL_FIXED_FROM_DOUBLE(d) ((wl_fixed_t)((d) * 256.0 + ((d) >= 0 ? 0.5 : -0.5)))

/* Pointer listener (version 1, extended in version 5) */
/*
 * Field order is the wire order — DO NOT REARRANGE. Missing callbacks
 * are NULL (set to NULL in the struct literal); libwayland skips them.
 *
 * The first 5 fields are in version 1 (enter, leave, motion, button, axis).
 * Fields 6-9 (frame, axis_source, axis_stop, axis_discrete) were added in v5.
 */
struct wl_pointer_listener {
    void (*enter)(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, struct wl_surface *surface,
                  wl_fixed_t surface_x, wl_fixed_t surface_y);
    void (*leave)(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, struct wl_surface *surface);
    void (*motion)(void *data, struct wl_pointer *wl_pointer,
                   uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y);
    void (*button)(void *data, struct wl_pointer *wl_pointer,
                   uint32_t serial, uint32_t time,
                   uint32_t button, uint32_t state);
    void (*axis)(void *data, struct wl_pointer *wl_pointer,
                 uint32_t time, uint32_t axis, wl_fixed_t value);
    /* since version 5 */
    void (*frame)(void *data, struct wl_pointer *wl_pointer);
    void (*axis_source)(void *data, struct wl_pointer *wl_pointer,
                        uint32_t axis_source);
    void (*axis_stop)(void *data, struct wl_pointer *wl_pointer,
                      uint32_t time, uint32_t axis);
    void (*axis_discrete)(void *data, struct wl_pointer *wl_pointer,
                          uint32_t axis, int32_t discrete);
};

/* Keyboard listener (version 1, extended in version 4) */
/*
 * Fields 1-5 are in version 1 (keymap, enter, leave, key, modifiers).
 * Field 6 (repeat_info) was added in v4.
 */
struct wl_keyboard_listener {
    void (*keymap)(void *data, struct wl_keyboard *wl_keyboard,
                   uint32_t format, int32_t fd, uint32_t size);
    void (*enter)(void *data, struct wl_keyboard *wl_keyboard,
                  uint32_t serial, struct wl_surface *surface,
                  struct wl_array *keys);
    void (*leave)(void *data, struct wl_keyboard *wl_keyboard,
                  uint32_t serial, struct wl_surface *surface);
    void (*key)(void *data, struct wl_keyboard *wl_keyboard,
                uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
    void (*modifiers)(void *data, struct wl_keyboard *wl_keyboard,
                      uint32_t serial, uint32_t mods_depressed,
                      uint32_t mods_latched, uint32_t mods_locked,
                      uint32_t group);
    /* since version 4 */
    void (*repeat_info)(void *data, struct wl_keyboard *wl_keyboard,
                        int32_t rate, int32_t delay);
};

/* Seat listener (v1: capabilities, v2: name) */
struct wl_seat_listener {
    void (*capabilities)(void *data, struct wl_seat *wl_seat,
                         uint32_t capabilities);
    void (*name)(void *data, struct wl_seat *wl_seat, const char *name);
};

/* Registry listener (v1) */
struct wl_registry_listener {
    void (*global)(void *data, struct wl_registry *wl_registry,
                   uint32_t name, const char *interface, uint32_t version);
    void (*global_remove)(void *data, struct wl_registry *wl_registry,
                          uint32_t name);
};

/* Enum constants (subset, only what we use) */

#define WL_POINTER_BUTTON_STATE_RELEASED 0u
#define WL_POINTER_BUTTON_STATE_PRESSED  1u

#define WL_POINTER_AXIS_VERTICAL_SCROLL   0u
#define WL_POINTER_AXIS_HORIZONTAL_SCROLL 1u

#define WL_KEYBOARD_KEY_STATE_RELEASED 0u
#define WL_KEYBOARD_KEY_STATE_PRESSED  1u

#define WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP 0u
#define WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1    1u

#ifdef __cplusplus
}
#endif

#endif /* IDK_WAYLAND_INPUT_TYPES_H */
