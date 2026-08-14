#ifndef IDK_WL_INPUT_INTERNAL_H
#define IDK_WL_INPUT_INTERNAL_H

#include "hook/wayland_internal.h"

/* Dir-local helpers: not part of the public symbol surface. */
#define IDK_INTERNAL __attribute__((visibility("hidden")))

IDK_INTERNAL int resolve_wayland_symbols(void);
IDK_INTERNAL int resolve_xkbcommon_symbols(void);

/* Keyboard (defined in kb.c, used by hotkey.c) */
IDK_INTERNAL uint32_t decode_keysym(uint32_t key);

/* Hotkey dispatch (defined in hotkey.c, used by kb.c's vtable) */
IDK_INTERNAL void wkb_key(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key,
                          uint32_t state);

/* Vendored cursor-shape protocol interfaces (defined in sidecar/marshal.c) */
IDK_INTERNAL extern const struct wl_interface g_wp_cursor_shape_manager_v1_interface;
IDK_INTERNAL extern const struct wl_interface g_wp_cursor_shape_device_v1_interface;

/* Sidecar listener vtables (defined in sidecar/cursor.c / sidecar/kb.c) */
IDK_INTERNAL extern const struct wl_pointer_listener g_sidecar_ptr_listener;
IDK_INTERNAL extern const struct wl_keyboard_listener g_sidecar_kb_listener;

#endif /* IDK_WL_INPUT_INTERNAL_H */
