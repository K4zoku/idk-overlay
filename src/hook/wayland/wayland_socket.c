#include "hook/wayland_internal.h"
#include "core/compositor_common.h"
#include "core/transport.h"

extern _Atomic int g_webview_dead;

static idk_transport_t g_input_tp;
static pthread_t g_accept_thread;
static pthread_mutex_t g_input_tp_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_input_inited = 0;
static _Atomic int g_accept_stop = 0;

int g_input_listen_fd = -1;
int g_client_fd = -1;
int g_accept_thread_started = 0;

static void *accept_thread_main(void *arg) {
    (void)arg;
    while (!atomic_load(&g_accept_stop)) {
        pthread_mutex_lock(&g_input_tp_lock);
        if (g_input_tp.ready) {
            pthread_mutex_unlock(&g_input_tp_lock);
            break;
        }
        int rc = idk_tp_accept(&g_input_tp);
        pthread_mutex_unlock(&g_input_tp_lock);

        if (rc < 0) {
            WLOG("accept_thread_main: idk_tp_accept fatal - input thread exiting");
            break;
        }
        if (rc == 1) {
            WLOG("webview connected to input transport");
            break;
        }
        for (int i = 0; i < 100 && !atomic_load(&g_accept_stop); i++)
            usleep(100);
    }
    return NULL;
}

int init_input_socket(void) {
    if (g_input_inited) {
        WLOG("input transport already initialized - sharing");
        return 0;
    }

    char path[256];
    const char *base = getenv("IDK_SOCKET");
    if (base && base[0]) {
        snprintf(path, sizeof(path), "%s-input", base);
    } else {
        idk_comp_get_default_socket_path(path, sizeof(path), 1);
    }
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        WERR("input socket path too long (%zu >= %zu): %s",
             strlen(path), sizeof(((struct sockaddr_un *)0)->sun_path), path);
        return -1;
    }

    if (idk_tp_init(&g_input_tp, IDK_TP_CONSUMER, path) != 0) {
        WERR("input transport init failed for %s", path);
        return -1;
    }

    g_input_inited = 1;
    g_input_listen_fd = 0;
    g_accept_thread_started = 1;

    if (pthread_create(&g_accept_thread, NULL, accept_thread_main, NULL) != 0) {
        WERR("pthread_create failed");
        idk_tp_destroy(&g_input_tp);
        g_input_inited = 0;
        g_input_listen_fd = -1;
        g_accept_thread_started = 0;
        return -1;
    }

    WLOG("input transport listening on %s (backend=%s)",
         path, g_input_tp.backend == IDK_TP_SHM ? "shm" : "socket");
    return 0;
}

void send_event_to_webview(const idk_input_event_t *ev) {
    if (g_webview_dead) return;
    pthread_mutex_lock(&g_input_tp_lock);
    if (!g_input_tp.ready) {
        idk_tp_accept(&g_input_tp);
    }
    if (!g_input_tp.ready) {
        pthread_mutex_unlock(&g_input_tp_lock);
        return;
    }
    int rc = idk_tp_send_input(&g_input_tp, ev);
    if (rc != 0) {
        int save_errno = errno;
        bool dead = (save_errno == EPIPE || save_errno == ECONNRESET ||
                     save_errno == ESHUTDOWN || save_errno == ECONNABORTED ||
                     save_errno == EBADF);
        if (dead) {
            idk_tp_disconnect_client(&g_input_tp);
            WLOG("send_event_to_webview: webview disconnected (errno=%d)", save_errno);
        } else if (save_errno != EAGAIN) {
            WLOG("send_event_to_webview: send failed (rc=%d, errno=%d)", rc, save_errno);
        }
    }
    pthread_mutex_unlock(&g_input_tp_lock);
}

void send_overlay_state(uint8_t visible) {
    idk_input_event_t ev = { 0 };
    ev.type = IDK_INPUT_OVERLAY;
    ev.u.overlay.visible = visible ? 1 : 0;
    IDK_LOG("wl-input", "send_overlay_state(%u) - sending to webview\n", visible);
    send_event_to_webview(&ev);
}

void send_capture_state(uint32_t capture) {
    idk_input_event_t ev = { 0 };
    ev.type  = IDK_INPUT_STATE;
    ev.flags = capture ? IDK_INPUT_FLAG_CAPTURE : 0;
    ev.mods  = (uint16_t)g_mods;
    IDK_LOG("wl-input", "send_capture_state(%u) flags=0x%x mods=0x%x - sending to webview\n",
            capture, ev.flags, ev.mods);
    send_event_to_webview(&ev);
}

void send_repeat_info(void) {
    idk_input_event_t ev = { 0 };
    ev.type = IDK_INPUT_REPEAT;
    /* Set CAPTURE flag so webview's capture-state tracking doesn't
     * misinterpret this as "capture disabled". The webview checks
     * (ev.flags & CAPTURE) on every event to track capture state -
     * REPEAT events are sent right after capture enable, so they
     * must carry the CAPTURE flag. */
    ev.flags = IDK_INPUT_FLAG_CAPTURE;
    ev.u.repeat.rate  = (uint16_t)g_repeat_rate;
    ev.u.repeat.delay = (uint16_t)g_repeat_delay;
    send_event_to_webview(&ev);
}

void teardown_input_socket(void) {
    if (g_accept_thread_started) {
        atomic_store(&g_accept_stop, 1);
        pthread_mutex_lock(&g_input_tp_lock);
        idk_tp_disconnect_client(&g_input_tp);
        pthread_mutex_unlock(&g_input_tp_lock);
        pthread_join(g_accept_thread, NULL);
        g_accept_thread_started = 0;
        atomic_store(&g_accept_stop, 0);
    }

    pthread_mutex_lock(&g_input_tp_lock);
    idk_tp_destroy(&g_input_tp);
    g_input_inited = 0;
    g_input_listen_fd = -1;
    pthread_mutex_unlock(&g_input_tp_lock);
}
