#include "hook/wayland_internal.h"
#include "core/compositor_common.h"

int g_input_listen_fd = -1;
int g_client_fd = -1;
static pthread_t g_accept_thread;
int g_accept_thread_started = 0;
static pthread_mutex_t g_client_fd_lock = PTHREAD_MUTEX_INITIALIZER;

static void *accept_thread_main(void *arg) {
    (void)arg;
    int listen_fd = g_input_listen_fd;
    while (1) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            /* Log fatal accept() errors before breaking — previously
             * the thread exited silently, leaving g_input_listen_fd
             * non-negative (so teardown_input_socket's check didn't
             * fire) and no new webview could ever connect. Without a
             * log, this is extremely hard to diagnose. */
            WERR("accept_thread_main: accept() fatal error: %s — input thread exiting",
                 strerror(errno));
            break;
        }
        pthread_mutex_lock(&g_client_fd_lock);
        if (g_client_fd >= 0)
            close(g_client_fd);
        g_client_fd = fd;
        pthread_mutex_unlock(&g_client_fd_lock);
        WLOG("webview connected to input socket (fd=%d)", fd);
    }
    return NULL;
}

int init_input_socket(void) {
    /* Use a buffer large enough for any reasonable XDG_RUNTIME_DIR +
     * "/idk-overlay-<pid>-input" suffix. sun_path is 108 bytes on
     * Linux, so we cap at 107 + NUL. If the path would exceed sun_path,
     * log an error and fail rather than silently truncating to a
     * different value than the frame socket (which uses a 512-byte
     * buffer) → input socket mismatch, webview can't connect. */
    char path[256];
    const char *base = getenv("IDK_SOCKET");
    if (base && base[0]) {
        snprintf(path, sizeof(path), "%s-input", base);
    } else {
        idk_comp_get_default_socket_path(path, sizeof(path), 1);
    }
    /* Validate against sun_path limit. sizeof(sun_path) is 108 on
     * Linux. If the path is too long, the frame socket (which uses
     * the same path minus "-input") would also be too long, so the
     * compositor's bind would have already failed. Still, guard
     * explicitly here for robustness. */
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        WERR("input socket path too long (%zu >= %zu): %s",
             strlen(path), sizeof(((struct sockaddr_un *)0)->sun_path), path);
        return -1;
    }

    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        WERR("input socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        WERR("input bind(%s) failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        WERR("input listen() failed: %s", strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }

    g_input_listen_fd = fd;
    if (pthread_create(&g_accept_thread, NULL, accept_thread_main, NULL) != 0) {
        WERR("pthread_create failed");
        close(fd);
        unlink(path);
        g_input_listen_fd = -1;
        return -1;
    }
    g_accept_thread_started = 1;

    WLOG("input socket listening on %s", path);
    return 0;
}

void send_event_to_webview(const idk_input_event_t *ev) {
    pthread_mutex_lock(&g_client_fd_lock);
    int fd = g_client_fd;
    pthread_mutex_unlock(&g_client_fd_lock);
    if (fd < 0) {
        /* No webview connected — drop silently (this is the normal
         * state before the webview connects, logging here would
         * spam). The caller's WLOG in the hotkey path handles
         * diagnostics. */
        return;
    }
    int rc = idk_ipc_send_input(fd, ev);
    if (rc != 0) {
        /* Send failed — likely EPIPE/ECONNRESET (webview crashed or
         * closed the connection). Close the dead fd so the next
         * accept_thread_main iteration can accept a new connection
         * cleanly. Without this, every subsequent event send fails
         * and logs until the webview reconnects and accept_thread
         * replaces g_client_fd (closing the old one). Between
         * disconnect and reconnect, all input events are silently
         * dropped and the log fills with "send failed" messages. */
        int save_errno = errno;
        bool dead = (save_errno == EPIPE || save_errno == ECONNRESET ||
                     save_errno == ESHUTDOWN || save_errno == ECONNABORTED ||
                     save_errno == EBADF);
        if (dead) {
            pthread_mutex_lock(&g_client_fd_lock);
            if (g_client_fd == fd) {
                close(g_client_fd);
                g_client_fd = -1;
            }
            pthread_mutex_unlock(&g_client_fd_lock);
            WLOG("send_event_to_webview: webview disconnected (errno=%d), fd closed", save_errno);
        } else {
            WLOG("send_event_to_webview: send failed (fd=%d, rc=%d, errno=%d)", fd, rc, save_errno);
        }
    }
}

void send_overlay_state(uint8_t visible) {
    idk_input_event_t ev = { 0 };
    ev.type = IDK_INPUT_OVERLAY;
    ev.u.overlay.visible = visible ? 1 : 0;
    IDK_LOG("wl-input", "send_overlay_state(%u) — sending to webview\n", visible);
    send_event_to_webview(&ev);
}

void send_capture_state(uint32_t capture) {
    idk_input_event_t ev = { 0 };
    ev.type  = IDK_INPUT_STATE;
    ev.flags = capture ? IDK_INPUT_FLAG_CAPTURE : 0;
    ev.mods  = (uint16_t)g_mods;
    IDK_LOG("wl-input", "send_capture_state(%u) flags=0x%x mods=0x%x — sending to webview\n",
            capture, ev.flags, ev.mods);
    send_event_to_webview(&ev);
}

void send_repeat_info(void) {
    idk_input_event_t ev = { 0 };
    ev.type = IDK_INPUT_REPEAT;
    /* Set CAPTURE flag so webview's capture-state tracking doesn't
     * misinterpret this as "capture disabled". The webview checks
     * (ev.flags & CAPTURE) on every event to track capture state —
     * REPEAT events are sent right after capture enable, so they
     * must carry the CAPTURE flag. */
    ev.flags = IDK_INPUT_FLAG_CAPTURE;
    ev.u.repeat.rate  = (uint16_t)g_repeat_rate;
    ev.u.repeat.delay = (uint16_t)g_repeat_delay;
    ev.u.repeat._p1 = 0;
    send_event_to_webview(&ev);
}

void teardown_input_socket(void) {
    if (g_accept_thread_started) {
        if (g_input_listen_fd >= 0) {
            shutdown(g_input_listen_fd, SHUT_RDWR);
            close(g_input_listen_fd);
            g_input_listen_fd = -1;
        }
        pthread_join(g_accept_thread, NULL);
        g_accept_thread_started = 0;
    }

    pthread_mutex_lock(&g_client_fd_lock);
    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
    }
    pthread_mutex_unlock(&g_client_fd_lock);
}
