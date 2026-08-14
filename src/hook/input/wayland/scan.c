#include "hook/wayland_internal.h"

/* Orig function pointers (set by syringe install) */
int (*orig_wl_proxy_add_listener)(struct wl_proxy *, void (**)(void), void *) = NULL;
int (*orig_wl_proxy_add_dispatcher)(struct wl_proxy *,
                                    int (*)(const void *, void *, uint32_t, const void *, const void *), const void *,
                                    void *) = NULL;

/* Direct implementation overwrite (bypass "already has listener") */
void *direct_overwrite_implementation(struct wl_proxy *proxy, void *new_impl, void *new_data, void **old_data_out) {
  void **impl_ptr = (void **)((char *)proxy + WL_PROXY_IMPL_OFFSET);
  void **data_ptr = (void **)((char *)proxy + WL_PROXY_DATA_OFFSET);

  void *old_impl = *impl_ptr;
  if (old_data_out)
    *old_data_out = *data_ptr;

  *impl_ptr = new_impl;
  *data_ptr = new_data;

  return old_impl;
}

/* Hook wl_proxy_add_listener */

int hook_wl_proxy_add_listener(struct wl_proxy *proxy, void (**impl)(void), void *data) {
  if (!real_wl_proxy_get_class || !real_wl_proxy_add_listener) {
    if (orig_wl_proxy_add_listener)
      return orig_wl_proxy_add_listener(proxy, impl, data);
    return -1;
  }

  const char *cls = real_wl_proxy_get_class(proxy);
  if (!cls) {
    return orig_wl_proxy_add_listener ? orig_wl_proxy_add_listener(proxy, impl, data)
                                      : real_wl_proxy_add_listener(proxy, impl, data);
  }

  static int s_kb_log_count = 0;
  static int s_ptr_log_count = 0;
  static int s_seat_log_count = 0;
  if (strcmp(cls, "wl_pointer") == 0 && s_ptr_log_count < 3) {
    s_ptr_log_count++;
    WLOG("add_listener: class=%s impl=%p data=%p", cls, (void *)impl, data);
  } else if (strcmp(cls, "wl_keyboard") == 0 && s_kb_log_count < 3) {
    s_kb_log_count++;
    WLOG("add_listener: class=%s impl=%p data=%p", cls, (void *)impl, data);
  } else if (strcmp(cls, "wl_seat") == 0 && s_seat_log_count < 3) {
    s_seat_log_count++;
    WLOG("add_listener: class=%s impl=%p data=%p", cls, (void *)impl, data);
  }

  if (strcmp(cls, "wl_pointer") == 0) {
    struct ptr_state *st = (struct ptr_state *)calloc(1, sizeof(*st));
    if (!st)
      goto passthrough;
    st->game = (const struct wl_pointer_listener *)impl;
    st->game_data = data;
    void *old_data = NULL;
    void *old_impl = direct_overwrite_implementation(proxy, (void *)&g_ptr_wrapper, st, &old_data);
    if (old_impl) {
      st->game = (const struct wl_pointer_listener *)old_impl;
      st->game_data = old_data;
    }
    g_game_pointer_proxy = proxy;
    WLOG("intercepted wl_pointer: game=%p data=%p proxy=%p (direct overwrite, old_impl=%p)", (void *)st->game,
         st->game_data, (void *)proxy, old_impl);
    return 0;
  }

  if (strcmp(cls, "wl_keyboard") == 0) {
    struct kb_state *st = (struct kb_state *)calloc(1, sizeof(*st));
    if (!st)
      goto passthrough;
    st->game = (const struct wl_keyboard_listener *)impl;
    st->game_data = data;
    void *old_data = NULL;
    void *old_impl = direct_overwrite_implementation(proxy, (void *)&g_kb_wrapper, st, &old_data);
    if (old_impl) {
      st->game = (const struct wl_keyboard_listener *)old_impl;
      st->game_data = old_data;
    }
    WLOG("intercepted wl_keyboard: game=%p data=%p proxy=%p (direct overwrite, old_impl=%p)", (void *)st->game,
         st->game_data, (void *)proxy, old_impl);
    return 0;
  }

passthrough:
  return orig_wl_proxy_add_listener ? orig_wl_proxy_add_listener(proxy, impl, data)
                                    : real_wl_proxy_add_listener(proxy, impl, data);
}

/* Hook wl_proxy_add_dispatcher (pass-through) */

int hook_wl_proxy_add_dispatcher(struct wl_proxy *proxy,
                                 int (*disp)(const void *, void *, uint32_t, const void *, const void *),
                                 const void *impl, void *data) {
  if (real_wl_proxy_get_class) {
    const char *cls = real_wl_proxy_get_class(proxy);
    if (cls && (strcmp(cls, "wl_pointer") == 0 || strcmp(cls, "wl_keyboard") == 0))
      WLOG("NOTE: %s uses dispatcher (GTK/Qt-style) - input hook "
           "won't intercept. Listener path is required.",
           cls);
  }
  return orig_wl_proxy_add_dispatcher
             ? orig_wl_proxy_add_dispatcher(proxy, disp, impl, data)
             : (real_wl_proxy_add_dispatcher ? real_wl_proxy_add_dispatcher(proxy, disp, impl, data) : -1);
}
