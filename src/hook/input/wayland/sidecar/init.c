#include "hook/wayland_internal.h"

#include "../internal.h"

/* Sidecar globals */
struct wl_display *g_sidecar_display = NULL;
struct wl_event_queue *g_sidecar_queue = NULL;
struct wl_seat *g_sidecar_seat = NULL;
struct wl_keyboard *g_sidecar_keyboard = NULL;
struct wl_pointer *g_sidecar_pointer = NULL;
struct wl_proxy *g_sidecar_cursor_shape_manager = NULL;
struct wl_proxy *g_sidecar_compositor = NULL;
struct wl_proxy *g_sidecar_shm = NULL;
int g_sidecar_initialized = 0;
int g_sidecar_ready = 0;

struct wl_proxy *g_cursor_shape_device = NULL;
struct wl_proxy *g_shape_device_pointer_proxy = NULL;

uint32_t g_sidecar_pointer_enter_serial = 0;
void *g_sidecar_surface = NULL;
wl_fixed_t g_sidecar_sx = 0;
wl_fixed_t g_sidecar_sy = 0;

/* my_wl_* protocol wrappers */

static struct wl_registry *my_wl_display_get_registry(struct wl_display *display) {
  if (!real_wl_proxy_marshal_constructor_versioned || !g_wl_registry_interface)
    return NULL;
  return (struct wl_registry *)real_wl_proxy_marshal_constructor_versioned(
      (struct wl_proxy *)display, WL_DISPLAY_GET_REGISTRY, g_wl_registry_interface,
      real_wl_proxy_get_version((struct wl_proxy *)display), NULL);
}

static void *my_wl_registry_bind(struct wl_registry *registry, uint32_t name, const struct wl_interface *interface,
                                 uint32_t version) {
  if (!real_wl_proxy_marshal_constructor_versioned || !g_wl_registry_interface)
    return NULL;
  return real_wl_proxy_marshal_constructor_versioned((struct wl_proxy *)registry, WL_REGISTRY_BIND, interface, version,
                                                     name, interface->name, version, NULL);
}

static struct wl_keyboard *my_wl_seat_get_keyboard(struct wl_seat *seat) {
  if (!real_wl_proxy_marshal_constructor_versioned || !g_wl_keyboard_interface)
    return NULL;
  return (struct wl_keyboard *)real_wl_proxy_marshal_constructor_versioned(
      (struct wl_proxy *)seat, WL_SEAT_GET_KEYBOARD, g_wl_keyboard_interface,
      real_wl_proxy_get_version((struct wl_proxy *)seat), NULL);
}

static struct wl_pointer *my_wl_seat_get_pointer(struct wl_seat *seat) {
  if (!real_wl_proxy_marshal_constructor_versioned || !g_wl_pointer_interface)
    return NULL;
  return (struct wl_pointer *)real_wl_proxy_marshal_constructor_versioned(
      (struct wl_proxy *)seat, WL_SEAT_GET_POINTER, g_wl_pointer_interface,
      real_wl_proxy_get_version((struct wl_proxy *)seat), NULL);
}

/* Sidecar seat listener */

static void sidecar_seat_capabilities(void *d, struct wl_seat *seat, uint32_t caps) {
  (void)d;
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_sidecar_pointer) {
    g_sidecar_pointer = my_wl_seat_get_pointer(seat);
    if (g_sidecar_pointer) {
      void *old_data = NULL;
      direct_overwrite_implementation((struct wl_proxy *)g_sidecar_pointer, (void *)&g_sidecar_ptr_listener, NULL,
                                      &old_data);
      WLOG("sidecar: wl_pointer bound + listener installed (old_impl=%p)", old_data);
    }
  }
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_sidecar_keyboard) {
    g_sidecar_keyboard = my_wl_seat_get_keyboard(seat);
    if (g_sidecar_keyboard) {
      void *old_data = NULL;
      direct_overwrite_implementation((struct wl_proxy *)g_sidecar_keyboard, (void *)&g_sidecar_kb_listener, NULL,
                                      &old_data);
      WLOG("sidecar: wl_keyboard bound + listener installed (old_impl=%p)", old_data);
    }
  }
}

static void sidecar_seat_name(void *d, struct wl_seat *seat, const char *name) {
  (void)d;
  (void)seat;
  (void)name;
}

static const struct wl_seat_listener g_sidecar_seat_listener = {
    .capabilities = sidecar_seat_capabilities,
    .name = sidecar_seat_name,
};

/* Sidecar registry listener */

static void sidecar_registry_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
  (void)d;
  (void)reg;
  WLOG("sidecar: registry global: iface=%s name=%u ver=%u", iface ? iface : "(null)", name, ver);
  if (strcmp(iface, "wl_seat") == 0 && !g_sidecar_seat && g_wl_seat_interface) {
    uint32_t bind_ver = ver < 5 ? ver : 5;
    WLOG("sidecar: binding wl_seat (name=%u ver=%u)", name, bind_ver);
    g_sidecar_seat = (struct wl_seat *)my_wl_registry_bind(reg, name, g_wl_seat_interface, bind_ver);
    WLOG("sidecar: wl_seat bound → %p", (void *)g_sidecar_seat);
    if (g_sidecar_seat && real_wl_proxy_add_listener)
      real_wl_proxy_add_listener((struct wl_proxy *)g_sidecar_seat, (void (**)(void))&g_sidecar_seat_listener, NULL);
  } else if (strcmp(iface, "wl_compositor") == 0 && !g_sidecar_compositor && g_wl_compositor_interface) {
    uint32_t bind_ver = ver < 4 ? ver : 4;
    g_sidecar_compositor = my_wl_registry_bind(reg, name, g_wl_compositor_interface, bind_ver);
    WLOG("sidecar: wl_compositor bound → %p (ver=%u)", (void *)g_sidecar_compositor, bind_ver);
  } else if (strcmp(iface, "wl_shm") == 0 && !g_sidecar_shm && g_wl_shm_interface) {
    g_sidecar_shm = my_wl_registry_bind(reg, name, g_wl_shm_interface, 1);
    WLOG("sidecar: wl_shm bound → %p", (void *)g_sidecar_shm);
  } else if (strcmp(iface, "wp_cursor_shape_manager_v1") == 0 && !g_sidecar_cursor_shape_manager) {
    if (idk_vk_layer_is_active()) {
      WLOG("sidecar: skipping wp_cursor_shape_manager_v1 (Vulkan layer mode)");
      return;
    }
    uint32_t bind_ver = ver < 2 ? ver : 2;
    g_sidecar_cursor_shape_manager =
        (struct wl_proxy *)my_wl_registry_bind(reg, name, &g_wp_cursor_shape_manager_v1_interface, bind_ver);
    WLOG("sidecar: wp_cursor_shape_manager_v1 bound → %p (ver=%u)", (void *)g_sidecar_cursor_shape_manager, bind_ver);
  }
}

static void sidecar_registry_global_remove(void *d, struct wl_registry *reg, uint32_t name) {
  (void)d;
  (void)reg;
  (void)name;
}

static const struct wl_registry_listener g_sidecar_registry_listener = {
    .global = sidecar_registry_global,
    .global_remove = sidecar_registry_global_remove,
};

/* Sidecar init */

int sidecar_init(struct wl_display *display) {
  if (g_sidecar_initialized)
    return 0;
  if (!display)
    return -1;

  static int s_sidecar_failed = 0;
  if (s_sidecar_failed)
    return -1;

  if (!real_wl_display_create_queue || !real_wl_proxy_create_wrapper || !real_wl_proxy_wrapper_destroy ||
      !real_wl_proxy_set_queue || !real_wl_display_roundtrip_queue || !real_wl_display_dispatch_queue_pending ||
      !real_wl_proxy_get_version || !real_wl_proxy_destroy || !real_wl_proxy_marshal_constructor_versioned ||
      !real_wl_proxy_marshal_flags || !g_wl_seat_interface || !g_wl_keyboard_interface || !g_wl_registry_interface) {
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

  real_wl_proxy_add_listener((struct wl_proxy *)registry, (void (**)(void))&g_sidecar_registry_listener, NULL);

  g_sidecar_initialized = 1;

  if (real_wl_display_roundtrip_queue) {
    real_wl_display_roundtrip_queue(display, g_sidecar_queue);
    g_sidecar_ready = 1;
  }

  WLOG("sidecar: initialized (seat=%p keyboard=%p ready=%d)", (void *)g_sidecar_seat, (void *)g_sidecar_keyboard,
       g_sidecar_ready);
  return 0;
}
