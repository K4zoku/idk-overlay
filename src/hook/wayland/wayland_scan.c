#include "hook/wayland_internal.h"

/* Intercepted proxy tracking */
struct wl_proxy *g_intercepted_proxies[MAX_INTERCEPTED];
int g_intercepted_count = 0;
static pthread_mutex_t g_scan_mutex = PTHREAD_MUTEX_INITIALIZER;

__thread int g_in_dispatch = 0;

/* Orig function pointers (set by syringe install) */
int (*orig_wl_proxy_add_listener)(struct wl_proxy *, void (**)(void), void *) = NULL;
int (*orig_wl_proxy_add_dispatcher)(struct wl_proxy *,
    int (*)(const void *, void *, uint32_t, const void *, const void *),
    const void *, void *) = NULL;

/* Direct implementation overwrite (bypass "already has listener") */
void *direct_overwrite_implementation(struct wl_proxy *proxy,
                                       void *new_impl, void *new_data,
                                       void **old_data_out) {
    void **impl_ptr = (void **)((char *)proxy + 8);
    void **data_ptr = (void **)((char *)proxy + 48);

    void *old_impl = *impl_ptr;
    if (old_data_out) *old_data_out = *data_ptr;

    *impl_ptr = new_impl;
    *data_ptr = new_data;

    return old_impl;
}

/* Hook wl_proxy_add_listener */

int hook_wl_proxy_add_listener(struct wl_proxy *proxy,
                                void (**impl)(void), void *data) {
    if (!real_wl_proxy_get_class || !real_wl_proxy_add_listener) {
        if (orig_wl_proxy_add_listener)
            return orig_wl_proxy_add_listener(proxy, impl, data);
        return -1;
    }

    const char *cls = real_wl_proxy_get_class(proxy);
    if (!cls) {
        return orig_wl_proxy_add_listener
            ? orig_wl_proxy_add_listener(proxy, impl, data)
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
        if (!st) goto passthrough;
        st->game      = (const struct wl_pointer_listener *)impl;
        st->game_data = data;
        void *old_data = NULL;
        void *old_impl = direct_overwrite_implementation(
            proxy, (void *)&g_ptr_wrapper, st, &old_data);
        if (old_impl) {
            st->game = (const struct wl_pointer_listener *)old_impl;
            st->game_data = old_data;
        }
        g_game_pointer_proxy = proxy;
        WLOG("intercepted wl_pointer: game=%p data=%p proxy=%p (direct overwrite, old_impl=%p)",
             (void *)st->game, st->game_data, (void *)proxy, old_impl);
        return 0;
    }

    if (strcmp(cls, "wl_keyboard") == 0) {
        struct kb_state *st = (struct kb_state *)calloc(1, sizeof(*st));
        if (!st) goto passthrough;
        st->game      = (const struct wl_keyboard_listener *)impl;
        st->game_data = data;
        void *old_data = NULL;
        void *old_impl = direct_overwrite_implementation(
            proxy, (void *)&g_kb_wrapper, st, &old_data);
        if (old_impl) {
            st->game = (const struct wl_keyboard_listener *)old_impl;
            st->game_data = old_data;
        }
        WLOG("intercepted wl_keyboard: game=%p data=%p proxy=%p (direct overwrite, old_impl=%p)",
             (void *)st->game, st->game_data, (void *)proxy, old_impl);
        return 0;
    }

passthrough:
    return orig_wl_proxy_add_listener
        ? orig_wl_proxy_add_listener(proxy, impl, data)
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
                 "won't intercept. Listener path is required.", cls);
    }
    return orig_wl_proxy_add_dispatcher
        ? orig_wl_proxy_add_dispatcher(proxy, disp, impl, data)
        : (real_wl_proxy_add_dispatcher
           ? real_wl_proxy_add_dispatcher(proxy, disp, impl, data)
           : -1);
}

/* Helpers */

static int is_already_intercepted(struct wl_proxy *proxy) {
    for (int i = 0; i < g_intercepted_count; i++) {
        if (g_intercepted_proxies[i] == proxy) return 1;
    }
    return 0;
}

static void mark_intercepted(struct wl_proxy *proxy) {
    if (g_intercepted_count < MAX_INTERCEPTED)
        g_intercepted_proxies[g_intercepted_count++] = proxy;
}

/* Scan display's proxy list for un-intercepted input devices */

void scan_and_intercept_input_proxies(struct wl_display *display) {
    if (!real_wl_proxy_get_class || !real_wl_proxy_get_version) return;

    if (g_in_dispatch) return;

    pthread_mutex_lock(&g_scan_mutex);

    struct wl_proxy *display_proxy = (struct wl_proxy *)display;
    struct wl_event_queue *queue = *(struct wl_event_queue **)((char *)display_proxy + 32);
    if (!queue) {
        pthread_mutex_unlock(&g_scan_mutex);
        return;
    }

    struct wl_list *head = (struct wl_list *)((char *)queue + WL_EVENT_QUEUE_PROXY_LIST_OFFSET);
    struct wl_list *item = head->next;

    int found = 0;
    unsigned int iterations = 0;
    while (item != head && iterations < (MAX_INTERCEPTED * 4) && found < MAX_INTERCEPTED) {
        iterations++;
        struct wl_proxy *proxy = (struct wl_proxy *)((char *)item - WL_PROXY_QUEUE_LINK_OFFSET);
        struct wl_list *next = item->next;
        if (next == NULL || next == head || next == item)
            break;
        item = next;

        const char *cls = real_wl_proxy_get_class(proxy);
        if (!cls) continue;

        void **impl_ptr = (void **)((char *)proxy + 8);
        if (*impl_ptr == (void *)&g_kb_wrapper ||
            *impl_ptr == (void *)&g_ptr_wrapper) continue;
        if (is_already_intercepted(proxy)) continue;
        if (proxy == (struct wl_proxy *)g_sidecar_keyboard) continue;

        if (strcmp(cls, "wl_keyboard") == 0) {
            struct kb_state *st = (struct kb_state *)calloc(1, sizeof(*st));
            if (!st) continue;
            void *old_data = NULL;
            void *old_impl = direct_overwrite_implementation(
                proxy, (void *)&g_kb_wrapper, st, &old_data);
            st->game = (const struct wl_keyboard_listener *)old_impl;
            st->game_data = old_data;
            mark_intercepted(proxy);
            WLOG("scan: intercepted game keyboard: proxy=%p game=%p data=%p",
                 (void *)proxy, (void *)st->game, st->game_data);
            found++;
        } else if (strcmp(cls, "wl_pointer") == 0) {
            struct ptr_state *st = (struct ptr_state *)calloc(1, sizeof(*st));
            if (!st) continue;
            void *old_data = NULL;
            void *old_impl = direct_overwrite_implementation(
                proxy, (void *)&g_ptr_wrapper, st, &old_data);
            st->game = (const struct wl_pointer_listener *)old_impl;
            st->game_data = old_data;
            mark_intercepted(proxy);
            g_game_pointer_proxy = proxy;
            WLOG("scan: intercepted game pointer: proxy=%p game=%p data=%p",
                 (void *)proxy, (void *)st->game, st->game_data);
            if (g_sidecar_pointer_enter_serial && st->game && st->game->enter) {
                WLOG("scan: synthetic enter for cursor state init "
                     "(serial=%u surface=%p)", g_sidecar_pointer_enter_serial,
                     g_sidecar_surface);
                g_last_enter_serial = g_sidecar_pointer_enter_serial;
                g_last_pointer_serial = g_sidecar_pointer_enter_serial;
                g_pointer_in_surface = 1;
                st->game->enter(st->game_data,
                    (struct wl_pointer *)proxy,
                    g_sidecar_pointer_enter_serial,
                    (struct wl_surface *)g_sidecar_surface,
                    g_sidecar_sx, g_sidecar_sy);
            }
            found++;
        }
    }

    pthread_mutex_unlock(&g_scan_mutex);
}
