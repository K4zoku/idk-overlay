/* Unix domain socket transport backend. */
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

#include "core/transport.h"
#include "core/log.h"

/* Internal state (owned by transport._rsv) */
/* Layout within _rsv[48]:
 *   [0..3]   int    state     - 0=init, 1=listening(consumer), 2=connected
 *   [4..7]   int    connect_retries  - remaining retries for producer
 *   [8..47]  char   pad[40]
 */

#define TP_S_STATE(rsv)          (*(int *)(rsv))
#define TP_S_CONNECT_RETRIES(rsv) (*(int *)((rsv) + 4))
#define TP_STATE_INIT    0
#define TP_STATE_LISTEN  1   /* consumer: socket created, listening */
#define TP_STATE_BOUND   2   /* consumer: socket bound before listen */
#define TP_STATE_READY   3   /* consumer: client accepted / producer: connected */

/* Helpers */

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return (fl < 0) ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Consumer init */

static int consumer_init(idk_transport_t *tp, const char *path) {
    /* Check if another instance already owns this socket */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
        int test = socket(AF_UNIX, SOCK_STREAM, 0);
        if (test >= 0) {
            struct sockaddr_un addr = { .sun_family = AF_UNIX };
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", path);
            if (connect(test, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                close(test);
                IDK_ERR("tp", "Another instance owns %s\n", path);
                return -1;
            }
            close(test);
        }
    }

    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_nonblock(fd);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    listen(fd, 4);

    tp->_server_fd = fd;
    tp->_client_fd = -1;
    TP_S_STATE(tp->_rsv) = TP_STATE_LISTEN;
    tp->ready = false;
    IDK_LOG("tp", "socket: listening on %s\n", path);
    return 0;
}

/* Producer init */

static int producer_init(idk_transport_t *tp, const char *path) {
    /* Try connect with retries */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t len = strlen(path);
    if (len >= sizeof(addr.sun_path)) { close(fd); return -1; }
    memcpy(addr.sun_path, path, len + 1);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        if (errno == ECONNREFUSED || errno == ENOENT) {
            /* Will retry in poll/recv - stash fd and path */
            close(fd);
            tp->_server_fd = -1;
            tp->_client_fd = -1;
            TP_S_STATE(tp->_rsv) = TP_STATE_INIT;
            TP_S_CONNECT_RETRIES(tp->_rsv) = 30;
            /* Store path in _rsv[8..] for retry */
            size_t cplen = len < 40 ? len : 39;
            memcpy(tp->_rsv + 8, path, cplen);
            tp->_rsv[8 + cplen] = '\0';
            tp->ready = false;
            IDK_LOG("tp", "socket: producer defer connect to %s\n", path);
            return 0;  /* not an error - will retry */
        }
        close(fd);
        return -1;
    }

    tp->_server_fd = -1;
    tp->_client_fd = fd;
    TP_S_STATE(tp->_rsv) = TP_STATE_READY;
    tp->ready = true;
    IDK_LOG("tp", "socket: connected to %s (fd=%d)\n", path, fd);
    return 0;
}

/* Interface implementation */

int tp_socket_init(idk_transport_t *tp, const char *name) {
    /* Preserve role/ready/backend set by idk_tp_init; only zero internal state. */
    idk_tp_role_t role = tp->role;
    uint8_t backend = tp->backend;
    memset(tp->_rsv, 0, sizeof(tp->_rsv));
    tp->_server_fd = -1;
    tp->_client_fd = -1;
    tp->ready = false;
    tp->role = role;
    tp->backend = backend;
    if (role == IDK_TP_CONSUMER)
        return consumer_init(tp, name);
    else
        return producer_init(tp, name);
}

void tp_socket_destroy(idk_transport_t *tp) {
    if (tp->_client_fd >= 0) { close(tp->_client_fd); tp->_client_fd = -1; }
    if (tp->_server_fd >= 0) { close(tp->_server_fd); tp->_server_fd = -1; }
    tp->ready = false;
}

void tp_socket_disconnect_client(idk_transport_t *tp) {
    /* Soft-disconnect: close the connected client fd but keep the
     * listen fd open so the next accept() can pick up a reconnecting
     * producer. The compositor's recv loop calls this on EPIPE/EOF
     * instead of idk_tp_destroy() (which would close the listen fd
     * and leave the overlay permanently dead). */
    if (tp->_client_fd >= 0) { close(tp->_client_fd); tp->_client_fd = -1; }
    tp->ready = false;
}

int tp_socket_accept(idk_transport_t *tp) {
    if (tp->_server_fd < 0) return -1;
    if (tp->_client_fd >= 0) return 1;  /* already accepted */

    int c = accept(tp->_server_fd, NULL, NULL);
    if (c < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    set_nonblock(c);
    tp->_client_fd = c;
    tp->ready = true;
    IDK_LOG("tp", "socket: client connected (fd=%d)\n", c);
    return 1;
}

int tp_socket_poll(idk_transport_t *tp) {
    if (tp->_client_fd < 0) {
        /* Producer mode: retry connect if deferred */
        if (tp->role == IDK_TP_PRODUCER && !tp->ready) {
            int *retries = &TP_S_CONNECT_RETRIES(tp->_rsv);
            if (*retries <= 0) return 0;
            (*retries)--;
            const char *path = (const char *)(tp->_rsv + 8);
            if (path[0] == '\0') return 0;
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return 0;
            struct sockaddr_un addr = { .sun_family = AF_UNIX };
            memcpy(addr.sun_path, path, strlen(path) + 1);
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                tp->_client_fd = fd;
                tp->ready = true;
                IDK_LOG("tp", "socket: connected to %s (fd=%d)\n", path, fd);
                return 0;  /* connected but no data yet */
            }
            close(fd);
            usleep(100000);  /* 100ms between retries */
            return 0;
        }
        return -1;
    }

    struct pollfd pfd = { .fd = tp->_client_fd, .events = POLLIN };
    int rc = poll(&pfd, 1, 0);
    if (rc < 0) return -1;
    return (pfd.revents & POLLIN) ? 1 : 0;
}

int tp_socket_recv(idk_transport_t *tp, idk_frame_header_t *hdr,
                   int fds[4], int *nfd) {
    if (tp->_client_fd < 0 || !hdr || !fds || !nfd) {
        errno = EINVAL;
        return -1;
    }

    char ctrl_buf[CMSG_SPACE(sizeof(int) * 4)] = { 0 };
    struct iovec iov = { .iov_base = hdr, .iov_len = sizeof(*hdr) };
    struct msghdr msgh = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl_buf, .msg_controllen = sizeof(ctrl_buf),
    };

    ssize_t n = recvmsg(tp->_client_fd, &msgh, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;  /* disconnect / error */
    }
    if (n == 0) return -1;                    /* EOF */
    if ((size_t)n < sizeof(*hdr)) return -1;  /* partial header */

    *nfd = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msgh); c;
         c = CMSG_NXTHDR(&msgh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int flen = (int)(c->cmsg_len - CMSG_LEN(0)) / (int)sizeof(int);
            if (flen > 4) flen = 4;
            memcpy(fds, CMSG_DATA(c), (size_t)flen * sizeof(int));
            *nfd = flen;
            break;
        }
    }

    return 1;
}

void tp_socket_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack) {
    if (tp->_client_fd < 0 || !ack) return;

    int fl = fcntl(tp->_client_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(tp->_client_fd, F_SETFL, fl | O_NONBLOCK);
    ssize_t n = write(tp->_client_fd, ack, sizeof(*ack));
    if (fl >= 0) fcntl(tp->_client_fd, F_SETFL, fl);

    /* On EPIPE/ECONNRESET, the peer is gone - close our side and mark
     * disconnected so idk_fs_is_connected() returns false and the
     * webview's Manager reconnect timer can kick in. Don't spam logs. */
    if (n < 0 && (errno == EPIPE || errno == ECONNRESET ||
                  errno == ESHUTDOWN || errno == ECONNABORTED)) {
        if (tp->ready) {
            tp->ready = false;
            close(tp->_client_fd);
            tp->_client_fd = -1;
        }
    }
}

int tp_socket_send(idk_transport_t *tp, const idk_frame_header_t *hdr,
                   const int *fds, int nfd) {
    if (tp->_client_fd < 0 || !hdr || !fds || nfd < 1 || nfd > 4) {
        errno = EINVAL;
        return -1;
    }

    struct iovec iov = { .iov_base = (void *)hdr, .iov_len = sizeof(*hdr) };
    char ctrl_buf[CMSG_SPACE(sizeof(int) * 4)] = { 0 };
    struct msghdr msgh = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl_buf,
        .msg_controllen = sizeof(ctrl_buf),
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfd);
    msgh.msg_controllen = cmsg->cmsg_len;

    ssize_t n = sendmsg(tp->_client_fd, &msgh, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        /* Fatal peer errors - disconnect immediately, no retry.
         * The webview Manager will reconnect via its 1s timer. */
        if (errno == EPIPE || errno == ECONNRESET ||
            errno == ESHUTDOWN || errno == ECONNABORTED || errno == EBADF) {
            if (tp->ready) {
                tp->ready = false;
                close(tp->_client_fd);
                tp->_client_fd = -1;
            }
            return -1;
        }
        IDK_ERR("tp", "sendmsg failed: %s\n", strerror(errno));
        return -1;
    }
    if ((size_t)n != sizeof(*hdr)) return -1;
    return 0;
}

int tp_socket_wait_ack(idk_transport_t *tp, idk_ack_msg_t *ack, int timeout_ms) {
    if (tp->_client_fd < 0 || !ack) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd pfd = { .fd = tp->_client_fd, .events = POLLIN };
    if (poll(&pfd, 1, timeout_ms) <= 0) return -1;
    if (!(pfd.revents & POLLIN)) return -1;

    size_t total = 0;
    while (total < sizeof(*ack)) {
        ssize_t n = read(tp->_client_fd, (char *)ack + total,
                         sizeof(*ack) - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

int tp_socket_send_request(idk_transport_t *tp, const idk_request_msg_t *req) {
    if (tp->_client_fd < 0 || !req) { errno = EINVAL; return -1; }
    ssize_t n = write(tp->_client_fd, req, sizeof(*req));
    if (n < 0) {
        if (errno == EPIPE || errno == ECONNRESET) {
            tp->ready = false; close(tp->_client_fd); tp->_client_fd = -1;
        }
        return -1;
    }
    return ((size_t)n == sizeof(*req)) ? 0 : -1;
}

int tp_socket_recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms) {
    if (tp->_client_fd < 0 || !req) { errno = EINVAL; return -1; }
    struct pollfd pfd = { .fd = tp->_client_fd, .events = POLLIN };
    if (poll(&pfd, 1, timeout_ms) <= 0) return -1;
    if (!(pfd.revents & POLLIN)) return -1;
    ssize_t n;
    do {
        n = read(tp->_client_fd, req, sizeof(*req));
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;
    return ((size_t)n == sizeof(*req)) ? 0 : -1;
}
