#ifndef IDK_WL_FNS_H
#define IDK_WL_FNS_H

#include "hook/wl_offsets.h"

/* Wayland proxy function table — X-macro pattern */
#define WL_FOREACH(F)                                                                                                  \
  F(int, wl_proxy_add_listener, (struct wl_proxy *, void (**)(void), void *))                                          \
  F(int, wl_proxy_add_dispatcher,                                                                                      \
    (struct wl_proxy *, int (*)(const void *, void *, uint32_t, const void *, const void *), const void *, void *))    \
  F(const char *, wl_proxy_get_class, (struct wl_proxy *))                                                             \
  F(const void *, wl_proxy_get_listener, (struct wl_proxy *))                                                          \
  F(struct wl_event_queue *, wl_display_create_queue, (struct wl_display *))                                           \
  F(struct wl_proxy *, wl_proxy_create_wrapper, (struct wl_proxy *))                                                   \
  F(void, wl_proxy_wrapper_destroy, (struct wl_proxy *))                                                               \
  F(void, wl_proxy_set_queue, (struct wl_proxy *, struct wl_event_queue *))                                            \
  F(int, wl_display_roundtrip_queue, (struct wl_display *, struct wl_event_queue *))                                   \
  F(int, wl_display_dispatch_queue_pending, (struct wl_display *, struct wl_event_queue *))                            \
  F(void, wl_event_queue_destroy, (struct wl_event_queue *))                                                           \
  F(uint32_t, wl_proxy_get_version, (struct wl_proxy *))                                                               \
  F(void, wl_proxy_destroy, (struct wl_proxy *))                                                                       \
  F(struct wl_proxy *, wl_proxy_marshal_constructor_versioned,                                                         \
    (struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, ...))                                         \
  F(struct wl_proxy *, wl_proxy_marshal_flags,                                                                         \
    (struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, ...))                               \
  F(struct wl_proxy *, wl_proxy_marshal_array_flags,                                                                   \
    (struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, union wl_argument *))

#define WL_TYPEDEF(ret, name, params) typedef ret(*name##_fn) params;
WL_FOREACH(WL_TYPEDEF)
#undef WL_TYPEDEF

/* Resolved wayland function pointers */
#define WL_EXTERN(ret, name, params) extern name##_fn real_##name;
WL_FOREACH(WL_EXTERN)
#undef WL_EXTERN

extern const struct wl_interface *g_wl_seat_interface;
extern const struct wl_interface *g_wl_keyboard_interface;
extern const struct wl_interface *g_wl_registry_interface;
extern const struct wl_interface *g_wl_pointer_interface;
extern const struct wl_interface *g_wl_buffer_interface;
extern const struct wl_interface *g_wl_compositor_interface;
extern const struct wl_interface *g_wl_shm_interface;
extern const struct wl_interface *g_wl_shm_pool_interface;
extern const struct wl_interface *g_wl_surface_interface;

extern void *g_wl_handle;

#endif /* IDK_WL_FNS_H */
