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

/* _rsv[56] layout: [0]=state, [4]=connect_retries, [8..47]=path */
#define TP_S_STATE(rsv)           (*(int *)(rsv))
#define TP_S_CONNECT_RETRIES(rsv) (*(int *)((rsv) + 4))
#define TP_S_ABSTRACT(rsv)        ((rsv)[7])
#define TP_STATE_INIT    0
#define TP_STATE_LISTEN  1
#define TP_STATE_BOUND   2
#define TP_STATE_READY   3
#define TP_S_PATH_CAP 40

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return (fl < 0) ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static bool is_abstract(const char *name) { return name && name[0] == '\0'; }

static socklen_t abstract_addrlen(const char *name) {
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name + 1));
}

static int consumer_init(idk_transport_t *tp, const char *path) {
    if (is_abstract(path)) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        set_nonblock(fd);

        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        memcpy(addr.sun_path, path, 1 + strlen(path + 1));
        socklen_t addrlen = abstract_addrlen(path);

        if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0) {
            close(fd);
            return -1;
        }
        listen(fd, 4);

        tp->_server_fd = fd;
        tp->_client_fd = -1;
        TP_S_STATE(tp->_rsv) = TP_STATE_LISTEN;
        tp->ready = false;
        IDK_LOG("tp", "socket: listening on abstract \\0%s\n", path + 1);
        return 0;
    }

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

static int producer_init(idk_transport_t *tp, const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int rc;
    bool abstr = is_abstract(path);
    size_t name_len;
    socklen_t addrlen;
    if (abstr) {
        name_len = 1 + strlen(path + 1);
        if (name_len >= sizeof(addr.sun_path)) { close(fd); return -1; }
        memcpy(addr.sun_path, path, name_len);
        addrlen = abstract_addrlen(path);
    } else {
        name_len = strlen(path);
        if (name_len >= sizeof(addr.sun_path)) { close(fd); return -1; }
        memcpy(addr.sun_path, path, name_len + 1);
        addrlen = sizeof(addr);
    }
    rc = connect(fd, (struct sockaddr *)&addr, addrlen);
    if (rc < 0) {
        if (errno == ECONNREFUSED || errno == ENOENT) {
            close(fd);
            tp->_server_fd = -1;
            tp->_client_fd = -1;
            TP_S_STATE(tp->_rsv) = TP_STATE_INIT;
            TP_S_CONNECT_RETRIES(tp->_rsv) = 30;
            TP_S_ABSTRACT(tp->_rsv) = abstr ? 1 : 0;
            size_t cap = TP_S_PATH_CAP - (abstr ? 0 : 1);
            size_t cplen = name_len < cap ? name_len : cap;
            memcpy(tp->_rsv + 8, path, cplen);
            if (!abstr) tp->_rsv[8 + cplen] = '\0';
            tp->ready = false;
            IDK_LOG("tp", "socket: producer defer connect to %s%s\n",
                    abstr ? "\\0" : "", abstr ? path + 1 : path);
            return 0;
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

int tp_socket_init(idk_transport_t *tp, const char *name) {
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
    if (tp->_client_fd >= 0) { close(tp->_client_fd); tp->_client_fd = -1; }
    tp->ready = false;
}

int tp_socket_accept(idk_transport_t *tp) {
    if (tp->_server_fd < 0) return -1;
    if (tp->_client_fd >= 0) return 1;

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
        if (tp->role == IDK_TP_PRODUCER && !tp->ready) {
            int *retries = &TP_S_CONNECT_RETRIES(tp->_rsv);
            if (*retries <= 0) return 0;
            (*retries)--;
            bool abstr = TP_S_ABSTRACT(tp->_rsv) != 0;
            const char *path = (const char *)(tp->_rsv + 8);
            if (!abstr && path[0] == '\0') return 0;
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return 0;
            struct sockaddr_un addr = { .sun_family = AF_UNIX };
            socklen_t addrlen;
            if (abstr) {
                size_t len = 1 + strlen(path + 1);
                memcpy(addr.sun_path, path, len);
                addrlen = abstract_addrlen(path);
            } else {
                memcpy(addr.sun_path, path, strlen(path) + 1);
                addrlen = sizeof(addr);
            }
            if (connect(fd, (struct sockaddr *)&addr, addrlen) == 0) {
                tp->_client_fd = fd;
                tp->ready = true;
                IDK_LOG("tp", "socket: connected to %s%s (fd=%d)\n",
                        abstr ? "\\0" : "", abstr ? path + 1 : path, fd);
                return 0;
            }
            close(fd);
            usleep(100000);
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
        return -1;
    }
    if (n == 0) return -1;
    if ((size_t)n < sizeof(*hdr)) return -1;

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

int tp_socket_drop_frame(idk_transport_t *tp) {
    idk_frame_header_t hdr;
    int fds[4], nfd = 0;
    int rc = tp_socket_recv(tp, &hdr, fds, &nfd);
    if (rc <= 0) return rc;
    for (int i = 0; i < nfd; i++) close(fds[i]);
    return 1;
}

void tp_socket_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack) {
    if (tp->_client_fd < 0 || !ack) return;

    ssize_t n = send(tp->_client_fd, ack, sizeof(*ack), MSG_NOSIGNAL);

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

int tp_socket_send_input(idk_transport_t *tp, const idk_input_event_t *ev) {
    if (!tp->ready || !ev) { errno = EINVAL; return -1; }
    if (tp->_client_fd < 0) { errno = ENOTCONN; return -1; }

    ssize_t n = send(tp->_client_fd, ev, sizeof(*ev),
                     MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        if (errno == EPIPE || errno == ECONNRESET ||
            errno == ESHUTDOWN || errno == ECONNABORTED || errno == EBADF) {
            tp->ready = false;
            close(tp->_client_fd);
            tp->_client_fd = -1;
        }
        return -1;
    }
    return ((size_t)n == sizeof(*ev)) ? 0 : -1;
}

int tp_socket_recv_input(idk_transport_t *tp, idk_input_event_t *ev) {
    if (!ev) { errno = EINVAL; return -1; }
    if (tp->_client_fd < 0) { errno = ENOTCONN; return -1; }

    ssize_t n;
    do {
        n = recv(tp->_client_fd, ev, sizeof(*ev), MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EPIPE || errno == ECONNRESET) {
            tp->ready = false;
            close(tp->_client_fd);
            tp->_client_fd = -1;
            return -1;
        }
        return -1;
    }
    if (n == 0) {
        tp->ready = false;
        close(tp->_client_fd);
        tp->_client_fd = -1;
        return -1;
    }
    if ((size_t)n != sizeof(*ev)) {
        return 0;
    }
    return 1;
}
