#include "hook/input_backend.h"
#include "hook/wayland_internal.h"

#include "../internal.h"

struct cursor_resource {
  struct wl_proxy *surface;
  struct wl_proxy *buffer;
  int released;
  struct cursor_resource *next;
};

static struct cursor_resource *g_custom_cursor;
static struct cursor_resource *g_retired_cursors;
static uint32_t g_applied_cursor_generation;

static void cursor_buffer_release(void *data, struct wl_buffer *buffer) {
  (void)buffer;
  ((struct cursor_resource *)data)->released = 1;
}

static const struct wl_buffer_listener g_cursor_buffer_listener = {
    .release = cursor_buffer_release,
};

static void destroy_cursor_resource(struct cursor_resource *cursor) {
  if (cursor->buffer)
    real_wl_proxy_marshal_flags(cursor->buffer, WL_BUFFER_DESTROY, NULL, real_wl_proxy_get_version(cursor->buffer),
                                WL_MARSHAL_FLAG_DESTROY);
  if (cursor->surface)
    real_wl_proxy_marshal_flags(cursor->surface, WL_SURFACE_DESTROY, NULL, real_wl_proxy_get_version(cursor->surface),
                                WL_MARSHAL_FLAG_DESTROY);
  free(cursor);
}

static void cleanup_retired_cursors(void) {
  struct cursor_resource **link = &g_retired_cursors;
  while (*link) {
    struct cursor_resource *cursor = *link;
    if (!cursor->released) {
      link = &cursor->next;
      continue;
    }
    *link = cursor->next;
    destroy_cursor_resource(cursor);
  }
}

static void retire_custom_cursor(void) {
  if (!g_custom_cursor)
    return;
  g_custom_cursor->next = g_retired_cursors;
  g_retired_cursors = g_custom_cursor;
  g_custom_cursor = NULL;
}

static struct cursor_resource *create_custom_cursor(const idk_cursor_update_t *cursor, const uint8_t *pixels) {
  if (!g_sidecar_compositor || !g_sidecar_shm || !g_wl_surface_interface || !g_wl_shm_pool_interface ||
      !g_wl_buffer_interface || !real_wl_proxy_marshal_constructor_versioned || !real_wl_proxy_marshal_flags)
    return NULL;
  size_t size = cursor->data_size;
  int fd = memfd_create("idk-cursor", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
    if (fd >= 0)
      close(fd);
    return NULL;
  }
  void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (memory == MAP_FAILED) {
    close(fd);
    return NULL;
  }
  memcpy(memory, pixels, size);
  struct wl_proxy *pool = real_wl_proxy_marshal_constructor_versioned(
      g_sidecar_shm, WL_SHM_CREATE_POOL, g_wl_shm_pool_interface, 1, NULL, fd, (int32_t)size);
  if (!pool) {
    munmap(memory, size);
    close(fd);
    return NULL;
  }
  struct wl_proxy *buffer = real_wl_proxy_marshal_constructor_versioned(
      pool, WL_SHM_POOL_CREATE_BUFFER, g_wl_buffer_interface, 1, NULL, 0, (int32_t)cursor->width,
      (int32_t)cursor->height, (int32_t)cursor->width * 4, WL_SHM_FORMAT_ARGB8888);
  real_wl_proxy_marshal_flags(pool, WL_SHM_POOL_DESTROY, NULL, real_wl_proxy_get_version(pool),
                              WL_MARSHAL_FLAG_DESTROY);
  munmap(memory, size);
  close(fd);
  if (!buffer)
    return NULL;
  struct wl_proxy *surface = real_wl_proxy_marshal_constructor_versioned(
      g_sidecar_compositor, WL_COMPOSITOR_CREATE_SURFACE, g_wl_surface_interface,
      real_wl_proxy_get_version(g_sidecar_compositor), NULL);
  if (!surface) {
    real_wl_proxy_marshal_flags(buffer, WL_BUFFER_DESTROY, NULL, real_wl_proxy_get_version(buffer),
                                WL_MARSHAL_FLAG_DESTROY);
    return NULL;
  }
  struct cursor_resource *resource = calloc(1, sizeof(*resource));
  if (!resource) {
    real_wl_proxy_marshal_flags(buffer, WL_BUFFER_DESTROY, NULL, real_wl_proxy_get_version(buffer),
                                WL_MARSHAL_FLAG_DESTROY);
    real_wl_proxy_marshal_flags(surface, WL_SURFACE_DESTROY, NULL, real_wl_proxy_get_version(surface),
                                WL_MARSHAL_FLAG_DESTROY);
    return NULL;
  }
  resource->surface = surface;
  resource->buffer = buffer;
  real_wl_proxy_add_listener(buffer, (void (**)(void))&g_cursor_buffer_listener, resource);
  real_wl_proxy_marshal_flags(surface, WL_SURFACE_ATTACH, NULL, real_wl_proxy_get_version(surface), 0, buffer, 0, 0);
  real_wl_proxy_marshal_flags(surface, WL_SURFACE_DAMAGE, NULL, real_wl_proxy_get_version(surface), 0, 0, 0,
                              (int32_t)cursor->width, (int32_t)cursor->height);
  if (real_wl_proxy_get_version(surface) >= 3) {
    int32_t scale = (cursor->scale + IDK_CURSOR_SCALE_BASE / 2) / IDK_CURSOR_SCALE_BASE;
    if (scale < 1)
      scale = 1;
    real_wl_proxy_marshal_flags(surface, WL_SURFACE_SET_BUFFER_SCALE, NULL, real_wl_proxy_get_version(surface), 0,
                                scale);
  }
  real_wl_proxy_marshal_flags(surface, WL_SURFACE_COMMIT, NULL, real_wl_proxy_get_version(surface), 0);
  return resource;
}

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
  real_wl_proxy_marshal_flags(device, WP_CURSOR_SHAPE_DEVICE_SET_SHAPE, NULL, real_wl_proxy_get_version(device), 0,
                              serial, shape);
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
    real_wl_proxy_marshal_flags(g_cursor_shape_device, 0, NULL, real_wl_proxy_get_version(g_cursor_shape_device),
                                WL_MARSHAL_FLAG_DESTROY);
    g_cursor_shape_device = NULL;
    g_shape_device_pointer_proxy = NULL;
  }
  uint32_t version = real_wl_proxy_get_version(g_sidecar_cursor_shape_manager);
  g_cursor_shape_device = real_wl_proxy_marshal_constructor_versioned(
      g_sidecar_cursor_shape_manager, 1, &g_wp_cursor_shape_device_v1_interface, version, NULL, p);
  if (!g_cursor_shape_device) {
    WERR("cursor: get_pointer failed");
    return;
  }
  g_shape_device_pointer_proxy = (struct wl_proxy *)p;
}

void idk_wayland_cursor_dispatch(void) {
  cleanup_retired_cursors();
  if (idk_vk_layer_is_active() || !g_captured || !g_game_pointer_proxy)
    return;
  static uint8_t pixels[IDK_CURSOR_MAX_BYTES];
  idk_cursor_update_t cursor;
  uint32_t generation;
  if (!idk_input_cursor_snapshot(g_applied_cursor_generation, &cursor, pixels, sizeof(pixels), &generation))
    return;
  uint32_t serial = g_last_enter_serial              ? g_last_enter_serial
                    : g_sidecar_pointer_enter_serial ? g_sidecar_pointer_enter_serial
                                                     : g_last_pointer_serial;
  if (!serial)
    return;
  if (!cursor.visible) {
    my_wl_pointer_set_cursor(g_game_pointer_proxy, serial, NULL, 0, 0);
    retire_custom_cursor();
  } else if (cursor.shape == IDK_CURSOR_CUSTOM) {
    struct cursor_resource *resource = create_custom_cursor(&cursor, pixels);
    if (!resource)
      return;
    int32_t hotspot_x = cursor.hotspot_x > 0 ? cursor.hotspot_x : 0;
    int32_t hotspot_y = cursor.hotspot_y > 0 ? cursor.hotspot_y : 0;
    my_wl_pointer_set_cursor(g_game_pointer_proxy, serial, (struct wl_surface *)resource->surface, hotspot_x,
                             hotspot_y);
    retire_custom_cursor();
    g_custom_cursor = resource;
  } else {
    sidecar_ensure_cursor_shape_device((struct wl_pointer *)g_game_pointer_proxy);
    if (!g_cursor_shape_device)
      return;
    uint32_t shape = cursor.shape;
    if (shape > IDK_CURSOR_ZOOM_OUT && real_wl_proxy_get_version(g_cursor_shape_device) < 2)
      shape = IDK_CURSOR_DEFAULT;
    my_wp_cursor_shape_device_set_shape(g_cursor_shape_device, serial, shape);
    retire_custom_cursor();
  }
  g_applied_cursor_generation = generation;
}

void idk_wayland_cursor_capture_changed(int captured) {
  g_applied_cursor_generation = 0;
  if (!captured)
    retire_custom_cursor();
  cleanup_retired_cursors();
}

void idk_wayland_cursor_shutdown(void) {
  if (g_custom_cursor)
    destroy_cursor_resource(g_custom_cursor);
  g_custom_cursor = NULL;
  while (g_retired_cursors) {
    struct cursor_resource *next = g_retired_cursors->next;
    destroy_cursor_resource(g_retired_cursors);
    g_retired_cursors = next;
  }
  g_applied_cursor_generation = 0;
}
