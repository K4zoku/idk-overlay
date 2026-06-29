#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "core/compositor.h"
#include "core/log.h"

/* Broker decision state — shared with overlay.c via extern.
 *   0=pending, 1=done-broker, 2=done-no-broker, 3=failed
 * Init to 2 so any code path without a broker thread sees "no broker". */
_Atomic int      g_broker_state = 2;
pthread_mutex_t  g_broker_lock  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t   g_broker_cond  = PTHREAD_COND_INITIALIZER;

/* ===== idk_compositor_t singleton ===== */

idk_compositor_t g_comp = {0};

/* ===== Resize debounce ===== */

bool idk_comp_notify_resize(int *game_w, int *game_h, bool *size_pending,
                            struct timespec *last_resize_ts,
                            int w, int h, const char *log_tag) {
    if (w < 1 || h < 1) return false;
    if (w != *game_w || h != *game_h) {
        IDK_LOG(log_tag, "resize: %dx%d -> %dx%d\n",
                *game_w, *game_h, w, h);
        *game_w = w;
        *game_h = h;
        *size_pending = true;
        clock_gettime(CLOCK_MONOTONIC, last_resize_ts);
        return true;
    }
    return false;
}

bool idk_comp_resize_stable(const struct timespec *last_resize_ts, int debounce_ms) {
    if (last_resize_ts->tv_sec == 0 && last_resize_ts->tv_nsec == 0) return true;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long delta_ms = (now.tv_sec - last_resize_ts->tv_sec) * 1000L
                  + (now.tv_nsec - last_resize_ts->tv_nsec) / 1000000L;
    return delta_ms >= debounce_ms;
}

/* ===== Path helpers ===== */

void idk_comp_get_runtime_dir(char *buf, size_t bufsz) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0]) {
        snprintf(buf, bufsz, "%s", xdg);
        size_t n = strlen(buf);
        while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
        return;
    }
    snprintf(buf, bufsz, "/tmp");
}

void idk_comp_get_default_socket_path(char *buf, size_t bufsz,
                                       int with_input_suffix) {
    char dir[PATH_MAX];
    idk_comp_get_runtime_dir(dir, sizeof(dir));
    if (with_input_suffix) {
        snprintf(buf, bufsz, "%s/idk-overlay-%d-input", dir, (int)getpid());
    } else {
        snprintf(buf, bufsz, "%s/idk-overlay-%d", dir, (int)getpid());
    }
}

void idk_comp_get_default_abstract_name(char *buf, size_t bufsz, int input) {
    snprintf(buf, bufsz, "idk_%s_%d", input ? "input" : "tp", (int)getpid());
}

void idk_comp_get_path(char *buf, size_t bufsz) {
    const char *abstr = getenv("IDK_TP_ABSTRACT");
    if (abstr && abstr[0]) {
        buf[0] = '\0';
        snprintf(buf + 1, bufsz - 1, "%s", abstr);
        return;
    }
    const char *env = getenv("IDK_SOCKET");
    if (env && env[0]) {
        snprintf(buf, bufsz, "%s", env);
    } else {
        idk_comp_get_default_socket_path(buf, bufsz, 0);
    }
}

/* ===== ACK builder ===== */

void idk_comp_build_ack(idk_ack_msg_t *msg, uint8_t ack,
                         int game_w, int game_h,
                         bool *size_pending,
                         const struct timespec *last_resize_ts,
                         int debounce_ms, const char *log_tag) {
    memset(msg, 0, sizeof(*msg));
    msg->ack = ack;
    if (*size_pending && idk_comp_resize_stable(last_resize_ts, debounce_ms)) {
        msg->w = game_w;
        msg->h = game_h;
        *size_pending = false;
        IDK_LOG(log_tag, "ACK with size %dx%d (ack=%d)\n",
                game_w, game_h, ack);
    } else if (*size_pending) {
        IDK_LOG(log_tag, "ACK without size (debouncing, ack=%d)\n", ack);
    }
}

/* ===== SHM mmap cache ===== */

void *idk_shm_cache_map(idk_shm_cache_t *c, int fd) {
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) return NULL;

    if (c->map && st.st_ino == c->ino && st.st_dev == c->dev)
        return c->map;

    if (c->map) { munmap(c->map, c->size); c->map = NULL; }

    off_t total = lseek(fd, 0, SEEK_END);
    if (total <= 0) total = 4096;
    c->size = (size_t)total;
    c->map = mmap(NULL, c->size, PROT_READ, MAP_SHARED, fd, 0);
    if (c->map == MAP_FAILED) { c->map = NULL; return NULL; }
    c->ino = st.st_ino;
    c->dev = st.st_dev;
    return c->map;
}

void idk_shm_cache_cleanup(idk_shm_cache_t *c) {
    if (c->map) { munmap(c->map, c->size); c->map = NULL; }
    c->size = 0; c->ino = 0; c->dev = 0;
}

/* ===== Cross-GPU dmabuf vendor detection ===== */

uint32_t idk_vk_vendor_to_drm(uint32_t vk_vendor) {
    switch (vk_vendor) {
        case 0x10DE: return 0x03;
        case 0x8086: return 0x01;
        case 0x1002: return 0x02;
        default:     return 0;
    }
}

/* ===== Shared compositor API ===== */

int idk_compositor_init(void) {
    if (g_comp.inited) return 0;

    /* Wait for broker decision (up to 5 s) — broker_connect_thread in
     * overlay.c will signal via g_broker_cond when state != 0. */
    pthread_mutex_lock(&g_broker_lock);
    while (atomic_load(&g_broker_state) == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;
        int rc = pthread_cond_timedwait(&g_broker_cond, &g_broker_lock, &ts);
        if (rc == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g_broker_lock);

    char path[512];
    idk_comp_get_path(path, sizeof(path));
    if (idk_tp_init(&g_comp.tp, IDK_TP_CONSUMER, path) != 0) return -1;
    idk_tp_accept(&g_comp.tp);
    g_comp.inited = true;
    return 0;
}

int idk_compositor_recv_frame(bool visible) {
    if (!g_comp.inited && idk_compositor_init() != 0) return -1;
    idk_tp_accept(&g_comp.tp);
    if (!g_comp.tp.ready) return 0;

    if (!visible) {
        while (1) {
            int rc = idk_tp_drop_frame(&g_comp.tp);
            if (rc <= 0) {
                if (rc < 0) {
                    g_comp.dmabuf_cache_id = 0;
                    idk_tp_disconnect_client(&g_comp.tp);
                }
                break;
            }
        }
        g_comp.was_hidden = true;
        return 0;
    }

    if (g_comp.was_hidden) {
        g_comp.was_hidden = false;
        idk_request_msg_t wake = {0};
        wake.type = IDK_REQUEST_NEXT_FRAME;
        idk_tp_send_request(&g_comp.tp, &wake);
    }

    int processed = 0;
    while (1) {
        idk_frame_header_t hdr;
        int fds[4], nfd = 0;
        int rc = idk_tp_recv(&g_comp.tp, &hdr, fds, &nfd);
        if (rc <= 0) {
            if (rc < 0) {
                idk_tp_disconnect_client(&g_comp.tp);
                g_comp.dmabuf_cache_id = 0;
            }
            break;
        }

        int dmabuf_fd = (nfd > 0) ? fds[0] : -1;

        if (processed > 0) {
            if (dmabuf_fd >= 0) close(dmabuf_fd);
            continue;
        }

        if (g_comp.dmabuf_fd[0] >= 0) {
            close(g_comp.dmabuf_fd[0]);
            g_comp.dmabuf_fd[0] = -1;
        }

        g_comp.hdr = hdr;
        g_comp.dmabuf_fd[0] = dmabuf_fd;
        g_comp.nfd = nfd;
        g_comp.frame_w = hdr.width;
        g_comp.frame_h = hdr.height;
        g_comp.has_frame = true;
        processed = 1;
        clock_gettime(CLOCK_MONOTONIC, &g_comp.last_frame_ts);
    }

    idk_request_msg_t req = {0};
    req.type = IDK_REQUEST_NEXT_FRAME;
    idk_tp_send_request(&g_comp.tp, &req);

    return processed ? 1 : 0;
}

void idk_compositor_send_ack(uint8_t code) {
    if (!g_comp.tp.ready) return;
    idk_ack_msg_t msg;
    idk_comp_build_ack(&msg, code,
                       g_comp.game_w, g_comp.game_h,
                       &g_comp.size_pending, &g_comp.last_resize_ts,
                       IDK_COMP_RESIZE_DEBOUNCE_MS, "comp");
    idk_tp_send_ack(&g_comp.tp, &msg);
}

void idk_compositor_send_request(void) {
    if (!g_comp.tp.ready) return;
    idk_request_msg_t req = {0};
    req.type = IDK_REQUEST_NEXT_FRAME;
    idk_tp_send_request(&g_comp.tp, &req);
}

void idk_compositor_notify_resize(int w, int h) {
    idk_comp_notify_resize(&g_comp.game_w, &g_comp.game_h, &g_comp.size_pending,
                          &g_comp.last_resize_ts, w, h, "comp");
}

int idk_compositor_has_frame(void) {
    return g_comp.has_frame ? 1 : 0;
}

void idk_compositor_shutdown(void) {
    if (g_comp.inited) {
        idk_tp_destroy(&g_comp.tp);
        g_comp.inited = false;
    }
}
