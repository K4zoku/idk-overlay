#include "hook/wayland_internal.h"

/* Sidecar dispatch pump */

void idk_wayland_input_sidecar_dispatch(void) {
  if (!g_sidecar_ready || !g_sidecar_display || !g_sidecar_queue)
    return;
  if (!orig_wl_display_dispatch_queue_pending)
    return;
  orig_wl_display_dispatch_queue_pending(g_sidecar_display, g_sidecar_queue);
}

/* Display connect hooks */

struct wl_display *(*orig_wl_display_connect)(const char *name) = NULL;
struct wl_display *(*orig_wl_display_connect_to_fd)(int fd) = NULL;

struct wl_display *hook_wl_display_connect(const char *name) {
  struct wl_display *display = orig_wl_display_connect ? orig_wl_display_connect(name) : NULL;
  if (display) {
    WLOG("wl_display_connect(\"%s\") → %p", name ? name : "(default)", (void *)display);
    sidecar_init(display);
  }
  return display;
}

struct wl_display *hook_wl_display_connect_to_fd(int fd) {
  struct wl_display *display = orig_wl_display_connect_to_fd ? orig_wl_display_connect_to_fd(fd) : NULL;
  if (display) {
    WLOG("wl_display_connect_to_fd(%d) → %p", fd, (void *)display);
    sidecar_init(display);
  }
  return display;
}

int (*orig_wl_display_dispatch_queue_pending)(struct wl_display *, struct wl_event_queue *) = NULL;

int hook_wl_display_dispatch_queue_pending(struct wl_display *display, struct wl_event_queue *queue) {
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

/* Marshal array flags hook (track cursor state) */

struct wl_proxy *(*orig_wl_proxy_marshal_array_flags)(struct wl_proxy *, uint32_t, const struct wl_interface *,
                                                      uint32_t, uint32_t, union wl_argument *) = NULL;

struct wl_proxy *hook_wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
                                                   const struct wl_interface *interface, uint32_t version,
                                                   uint32_t flags, union wl_argument *args) {
  if (proxy && proxy == g_game_pointer_proxy && opcode == WL_POINTER_SET_CURSOR && args && !g_captured)
    g_game_cursor_hidden = (args[1].o == NULL);
  if (orig_wl_proxy_marshal_array_flags)
    return orig_wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, args);
  if (real_wl_proxy_marshal_array_flags)
    return real_wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, args);
  return NULL;
}

/* Proxy destroy hook (cleanup cursor shape device) */

void (*orig_wl_proxy_destroy)(struct wl_proxy *) = NULL;

void hook_wl_proxy_destroy(struct wl_proxy *proxy) {
  if (proxy && proxy == g_game_pointer_proxy) {
    WLOG("wl_proxy_destroy: game pointer %p - destroying cursor shape device", (void *)proxy);
    if (g_cursor_shape_device) {
      real_wl_proxy_marshal_flags(g_cursor_shape_device, 0, NULL, 2, WL_MARSHAL_FLAG_DESTROY);
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
