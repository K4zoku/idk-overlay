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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>

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

/* ── Socket layer ─────────────────────────────────────────────────────── */

int idk_comp_sock_init(int *out_listen_fd, char *path, size_t path_sz,
                       const char *log_tag) {
    const char *env = getenv("IDK_SOCKET");
    if (env && env[0]) {
        snprintf(path, path_sz, "%s", env);
    } else {
        snprintf(path, path_sz, "/tmp/idk-overlay-%d", getpid());
    }

    /* Check if another instance already owns this socket */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
        int test = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", path);
        if (connect(test, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(test);
            IDK_ERR(log_tag, "Another instance owns socket %s\n", path);
            return -1;
        }
        close(test);
    }

    unlink(path);
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;

    int fl = fcntl(listen_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(listen_fd, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", path);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        return -1;
    }
    listen(listen_fd, 4);

    *out_listen_fd = listen_fd;
    IDK_LOG(log_tag, "Listening on %s\n", path);
    return 0;
}

int idk_comp_sock_accept(int listen_fd, int *out_client_fd, const char *log_tag) {
    if (*out_client_fd >= 0) return 0;
    if (listen_fd < 0) return -1;
    int c = accept(listen_fd, NULL, NULL);
    if (c >= 0) {
        int fl = fcntl(c, F_GETFL, 0);
        if (fl >= 0) fcntl(c, F_SETFL, fl | O_NONBLOCK);
        *out_client_fd = c;
        IDK_LOG(log_tag, "Client connected (fd=%d)\n", c);
        return 1;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

int idk_comp_recv_frame(int client_fd, idk_frame_header_t *out_hdr,
                        int *out_fd, const char *log_tag) {
    (void)log_tag;  /* reserved for future logging */
    struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return 0;

    char ctrl_buf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = out_hdr, .iov_len = sizeof(*out_hdr) };
    struct msghdr msgh = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl_buf, .msg_controllen = sizeof(ctrl_buf),
    };

    ssize_t n = recvmsg(client_fd, &msgh, MSG_DONTWAIT);
    if (n < (ssize_t)sizeof(*out_hdr)) {
        if (n == 0) return -1;  /* peer disconnected */
        return 0;
    }

    int fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msgh); c; c = CMSG_NXTHDR(&msgh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
            break;
        }
    }
    if (fd < 0) return 0;

    *out_fd = fd;
    return 1;
}

void idk_comp_send_ack(int client_fd, uint8_t ack,
                       int game_w, int game_h,
                       bool *size_pending,
                       const struct timespec *last_resize_ts,
                       int debounce_ms, const char *log_tag) {
    if (client_fd < 0) return;
    struct idk_ack_msg msg = {0};
    msg.ack = ack;
    if (*size_pending && idk_comp_resize_stable(last_resize_ts, debounce_ms)) {
        msg.w = game_w;
        msg.h = game_h;
        *size_pending = false;
        IDK_LOG(log_tag, "ACK with size %dx%d (ack=%d)\n",
                game_w, game_h, ack);
    } else if (*size_pending) {
        IDK_LOG(log_tag, "ACK without size (debouncing, ack=%d)\n", ack);
    }
    int fl = fcntl(client_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(client_fd, F_SETFL, fl | O_NONBLOCK);
    write(client_fd, &msg, sizeof(msg));
    if (fl >= 0) fcntl(client_fd, F_SETFL, fl);
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
