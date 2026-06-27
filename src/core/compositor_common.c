/*
 * compositor_common.c — shared code between GL and Vulkan compositors.
 *
 * See include/core/compositor_common.h for API docs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>  /* PATH_MAX */
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>

#include "core/compositor_common.h"
#include "core/log.h"

/* ── Resize debounce ──────────────────────────────────────────────────── */

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

/* ── Path helpers ────────────────────────────────────────────────────── */

void idk_comp_get_runtime_dir(char *buf, size_t bufsz) {
    /* XDG_RUNTIME_DIR is the correct per-user runtime location:
     *   - 0700-per-user tmpfs (no token auth needed)
     *   - cleaned up automatically on logout (systemd `tmpfiles.d`)
     *   - backed by tmpfs, so socket/SHM I/O is RAM-speed
     * Fall back to /tmp if unset (e.g. minimal containers, non-systemd
     * init, ssh sessions without PAM setting XDG_RUNTIME_DIR). */
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0]) {
        snprintf(buf, bufsz, "%s", xdg);
        /* Strip a trailing slash so callers can append "/foo" cleanly. */
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

void idk_comp_get_path(char *buf, size_t bufsz) {
    const char *env = getenv("IDK_SOCKET");
    if (env && env[0]) {
        snprintf(buf, bufsz, "%s", env);
    } else {
        idk_comp_get_default_socket_path(buf, bufsz, 0);
    }
}

/* ── ACK builder ──────────────────────────────────────────────────────── */

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

/* ── SHM mmap cache ──────────────────────────────────────────────────── */

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

/* ── Cross-GPU dmabuf vendor detection ────────────────────────────────── */

uint32_t idk_vk_vendor_to_drm(uint32_t vk_vendor) {
    switch (vk_vendor) {
        case 0x10DE: return 0x03;  /* NVIDIA */
        case 0x8086: return 0x01;  /* Intel */
        case 0x1002: return 0x02;  /* AMD */
        default: return 0;         /* unknown */
    }
}
