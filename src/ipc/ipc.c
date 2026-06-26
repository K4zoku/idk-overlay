/* ipc.c — Unix domain socket IPC for idk-overlay.
 *
 * Implements frame send/recv (24-byte header + fd via SCM_RIGHTS) and
 * input event send/recv (16-byte event, no fd).
 *
 * See include/public/idk_ipc.h for the wire format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#include "public/idk_ipc.h"
#include "core/log.h"

int idk_ipc_connect(const char *sockpath, int *out_fd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t len = strlen(sockpath);
    if (len >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, sockpath, len + 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == ECONNREFUSED) {
            close(fd);
            *out_fd = -1;
            return -1;
        }
        close(fd);
        return -1;
    }

    *out_fd = fd;
    return 0;
}

void idk_ipc_close(int fd) {
    if (fd >= 0) close(fd);
}

int idk_ipc_send_frame(int socket_fd, const idk_frame_header_t *hdr, int fd) {
    if (socket_fd < 0 || !hdr) {
        errno = EINVAL;
        return -1;
    }

    struct iovec iov = { .iov_base = (void *)hdr, .iov_len = sizeof(*hdr) };
    char ctrl_buf[CMSG_SPACE(sizeof(int))] = { 0 };
    struct msghdr msgh = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buf,
        .msg_controllen = sizeof(ctrl_buf),
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    msgh.msg_controllen = cmsg->cmsg_len;

    ssize_t n = sendmsg(socket_fd, &msgh, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        IDK_ERR("ipc", "sendmsg failed: %s\n", strerror(errno));
        return -1;
    }
    if ((size_t)n != sizeof(*hdr)) {
        return -1;
    }
    return 0;
}

int idk_ipc_recv_frame(int socket_fd, idk_frame_header_t *hdr, int *out_fd) {
    if (socket_fd < 0 || !hdr || !out_fd) {
        errno = EINVAL;
        return -1;
    }
    *out_fd = -1;

    char ctrl_buf[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec iov = { .iov_base = hdr, .iov_len = sizeof(*hdr) };
    struct msghdr msgh = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buf,
        .msg_controllen = sizeof(ctrl_buf),
    };

    struct pollfd pfd = { .fd = socket_fd, .events = POLLIN | POLLHUP };
    if (poll(&pfd, 1, 2000) <= 0) {
        return -1;
    }
    if (!(pfd.revents & (POLLIN | POLLHUP))) {
        return -1;
    }

    ssize_t n = recvmsg(socket_fd, &msgh, 0);
    if (n <= 0) {
        return -1;
    }
    if ((size_t)n < sizeof(*hdr)) {
        /* Partial header — protocol violation. */
        return -1;
    }

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh); cmsg;
         cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
            break;
        }
    }

    if (*out_fd < 0) {
        return -1;
    }

    return 0;
}

int idk_ipc_send_input(int socket_fd, const idk_input_event_t *ev) {
    if (socket_fd < 0 || !ev) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n = send(socket_fd, ev, sizeof(*ev),
                     MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        IDK_ERR("ipc", "send_input failed: %s\n", strerror(errno));
        return -1;
    }
    if ((size_t)n != sizeof(*ev)) {
        return -1;
    }
    return 0;
}

int idk_ipc_recv_input(int socket_fd, idk_input_event_t *ev, int flags) {
    if (socket_fd < 0 || !ev) {
        errno = EINVAL;
        return -1;
    }

    size_t total = 0;
    while (total < sizeof(*ev)) {
        ssize_t n = recv(socket_fd, (char *)ev + total,
                         sizeof(*ev) - total, flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        total += (size_t)n;
        flags = 0;  /* blocking after first chunk */
    }

    /* Validate type is in range. */
    if (ev->type < IDK_INPUT_KEY || ev->type > IDK_INPUT_REPEAT) {
        errno = EBADMSG;
        return -1;
    }

    return 0;
}
