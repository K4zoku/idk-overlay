/*
 * ipc.c — Unix domain socket IPC with SCM_RIGHTS for dmabuf fd passing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#include "idk_ipc.h"

typedef struct idk_frame_hdr {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t num_planes;
    uint32_t pid;
    uint32_t reserved;
    uint32_t checksum;
} idk_frame_hdr_t;

static uint32_t crc32_simple(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

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

int idk_ipc_send_frame(int socket_fd, const void *info, size_t info_len,
                       int dmabuf_fd) {
    if (socket_fd < 0 || info_len < sizeof(idk_frame_hdr_t)) {
        errno = EINVAL;
        return -1;
    }

    idk_frame_hdr_t send_hdr;
    memcpy(&send_hdr, info, sizeof(send_hdr));
    send_hdr.checksum = crc32_simple(info, info_len - sizeof(uint32_t));

    struct msghdr msgh = { 0 };
    struct iovec iov = { .iov_base = &send_hdr, .iov_len = info_len };
    char ctrl_buf[CMSG_SPACE(sizeof(int))] = { 0 };

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = ctrl_buf;
    msgh.msg_controllen = sizeof(ctrl_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &dmabuf_fd, sizeof(int));
    msgh.msg_controllen = cmsg->cmsg_len;

    ssize_t n = sendmsg(socket_fd, &msgh, 0);
    if (n < 0) {
        fprintf(stderr, "[idk] sendmsg failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int idk_ipc_recv_frame(int socket_fd, void *info, size_t info_len,
                       int *out_fd) {
    if (info_len < sizeof(idk_frame_hdr_t)) {
        errno = EINVAL;
        return -1;
    }

    char ctrl_buf[CMSG_SPACE(sizeof(int))] = { 0 };
    struct msghdr msgh = { 0 };
    struct iovec iov = { .iov_base = info, .iov_len = info_len };

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = ctrl_buf;
    msgh.msg_controllen = sizeof(ctrl_buf);

    struct pollfd pfd = { .fd = socket_fd, .events = POLLIN | POLLHUP };
    if (poll(&pfd, 1, 2000) <= 0) {
        return -1;
    }
    /* Data available or peer closed — try to read either way */
    if (!(pfd.revents & (POLLIN | POLLHUP))) {
        return -1;
    }

    ssize_t n = recvmsg(socket_fd, &msgh, 0);
    if (n <= 0) {
        if (n == 0) return -1;
        return -1;
    }

    *out_fd = -1;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh); cmsg;
         cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
        }
    }

    if (*out_fd < 0) {
        return -1;
    }

    uint32_t *checksum = (uint32_t *)((char *)info + info_len - sizeof(uint32_t));
    uint32_t expected = crc32_simple(info, info_len - sizeof(uint32_t));
    if (*checksum != expected) {
        close(*out_fd);
        *out_fd = -1;
        return -1;
    }

    return 0;
}
