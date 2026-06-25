/* idk_fs.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include "public/idk_fs.h"
#include "public/idk_ipc.h"
#include "core/log.h"

/* ── Internal state ── */

static int g_sock_fd = -1;
static char g_sock_path[IDK_IPC_SOCKNAME_MAX];

static uint32_t crc32_simple(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint8_t)p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

static void build_frame_hdr(const idk_fs_frame_t *frame,
                            uint8_t *hdr, size_t hdr_size) {
    if (hdr_size < sizeof(uint32_t) * 8) return;

    uint32_t *fields = (uint32_t *)hdr;
    fields[0] = frame->width;
    fields[1] = frame->height;
    fields[2] = frame->stride;       /* DMABUF: stride in bytes, SHM: 0 */
    fields[3] = frame->format;       /* DMABUF: DRM fourcc, SHM: 0 */
    fields[4] = (uint32_t)frame->id; /* num_planes → overlay ID */
    fields[5] = frame->width * frame->height * 4; /* pid → pixel byte size */
    fields[6] = (uint32_t)frame->visible          /* reserved → visibility (low byte) */
              | ((uint32_t)frame->type << 8);     /*            frame type (high byte) */
    /* fields[7] = checksum (filled by caller) */

    /* Write modifier at bytes 32-39 (after the 32-byte header).
     * The compositor reads 64 bytes, so this fits in the existing buffer. */
    if (hdr_size >= 40) {
        uint64_t *mod = (uint64_t *)(hdr + 32);
        *mod = frame->modifier;
    }
}

/* SHM helpers */

static int copy_to_shm(const void *src, size_t size) {
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/idk-framesource-%d", (int)getpid());

    int fd = shm_open(shm_name, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        IDK_ERR("fs", "shm_open failed: %s\n", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, size) < 0) {
        IDK_ERR("fs", "ftruncate failed: %s\n", strerror(errno));
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }

    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        IDK_ERR("fs", "mmap failed: %s\n", strerror(errno));
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }

    memcpy(map, src, size);
    munmap(map, size);

    return fd;
}

int idk_fs_init(const char *sockpath, int reuse_fd) {
    return idk_fs_init2(sockpath, reuse_fd, 30);
}

/* Variant with configurable retry count for the connect() loop.
 * 0 retries = single attempt (non-blocking — for use in event loops).
 * 30 retries = ~3s total (legacy behavior, blocks the caller). */
int idk_fs_init2(const char *sockpath, int reuse_fd, int retries) {
    if (!sockpath) {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(sockpath);
    if (len >= IDK_IPC_SOCKNAME_MAX) {
        errno = ERANGE;
        return -1;
    }
    strncpy(g_sock_path, sockpath, IDK_IPC_SOCKNAME_MAX);
    g_sock_path[IDK_IPC_SOCKNAME_MAX - 1] = '\0';

    if (!reuse_fd) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            IDK_ERR("fs", "socket() failed: %s\n", strerror(errno));
            return -1;
        }

        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        memcpy(addr.sun_path, sockpath, len + 1);

        int connect_ret = -1;
        for (int i = 0; i <= retries; i++) {
            connect_ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
            if (connect_ret == 0) break;
            if (errno != ECONNREFUSED && errno != ENOENT) {
                IDK_ERR("fs", "connect(%s) failed: %s\n",
                        sockpath, strerror(errno));
                close(fd);
                return -1;
            }
            if (i < retries) usleep(100000);
        }

        if (connect_ret < 0) {
            if (retries > 0 || (errno != ECONNREFUSED && errno != ENOENT)) {
                IDK_ERR("fs",
                        "connect(%s) failed after %d retries: %s\n",
                        sockpath, retries, strerror(errno));
            }
            close(fd);
            return -1;
        }

        g_sock_fd = fd;
        IDK_LOG("fs", "Connected to %s (fd=%d)\n", sockpath, fd);
    } else {
        g_sock_fd = reuse_fd;
        IDK_LOG("fs", "Reusing fd=%d for %s\n", reuse_fd, sockpath);
    }

    return 0;
}

int idk_fs_get_fd(void) {
    return g_sock_fd;
}

void idk_fs_shutdown(void) {
    if (g_sock_fd >= 0) {
        close(g_sock_fd);
        g_sock_fd = -1;
    }
}

int idk_fs_send_frame(int data_fd, const idk_fs_frame_t *frame) {
    if (g_sock_fd < 0) {
        errno = ENOTCONN;
        return -1;
    }
    if (!frame) {
        errno = EINVAL;
        return -1;
    }

    uint8_t hdr[40] = { 0 };
    build_frame_hdr(frame, hdr, sizeof(hdr));

    uint32_t checksum = crc32_simple(hdr, sizeof(hdr) - sizeof(uint32_t) - 8);
    ((uint32_t *)hdr)[7] = checksum;

    struct iovec iov = { .iov_base = hdr, .iov_len = sizeof(hdr) };
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
    memcpy(CMSG_DATA(cmsg), &data_fd, sizeof(int));
    msgh.msg_controllen = cmsg->cmsg_len;

    ssize_t n = sendmsg(g_sock_fd, &msgh, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN || errno == ESHUTDOWN) {
            IDK_ERR("fs", "sendmsg: peer closed (errno=%d: %s) — shutting down socket\n",
                    errno, strerror(errno));
            idk_fs_shutdown();
        } else {
            IDK_ERR("fs", "sendmsg failed: %s\n", strerror(errno));
        }
        return -1;
    }

    IDK_LOG("fs",
            "Frame sent: %dx%d stride=%u fourcc=0x%x id=%d visible=%d fd=%d\n",
            frame->width, frame->height, frame->stride, frame->format,
            frame->id, frame->visible, data_fd);
    return 0;
}

int idk_fs_send_pixels(const void *pixels, const idk_fs_frame_t *frame) {
    if (!pixels || !frame) {
        errno = EINVAL;
        return -1;
    }

    size_t pixel_size = (size_t)frame->width * (size_t)frame->height * 4;
    int shm_fd = copy_to_shm(pixels, pixel_size);
    if (shm_fd < 0) {
        IDK_ERR("fs", "copy_to_shm failed\n");
        return -1;
    }

    int rc = idk_fs_send_frame(shm_fd, frame);
    close(shm_fd);

    usleep(50000);

    return rc;
}

int idk_fs_send_dma_buf(const int *dma_buf_fds, const idk_fs_frame_t *frame) {
    if (g_sock_fd < 0) {
        errno = ENOTCONN;
        return -1;
    }
    if (!frame || !dma_buf_fds || (size_t)frame->nfd == 0 || (size_t)frame->nfd > 4) {
        errno = EINVAL;
        return -1;
    }

    uint8_t hdr[40] = { 0 };
    build_frame_hdr(frame, hdr, sizeof(hdr));

    uint32_t checksum = crc32_simple(hdr, sizeof(hdr) - sizeof(uint32_t) - 8);
    ((uint32_t *)hdr)[7] = checksum;

    struct iovec iov = { .iov_base = hdr, .iov_len = sizeof(hdr) };
    char ctrl_buf[CMSG_SPACE(sizeof(int) * 4)] = { 0 };
    struct msghdr msgh = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buf,
        .msg_controllen = sizeof(ctrl_buf),
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * frame->nfd);
    memcpy(CMSG_DATA(cmsg), dma_buf_fds, sizeof(int) * frame->nfd);
    msgh.msg_controllen = cmsg->cmsg_len;

    ssize_t n = sendmsg(g_sock_fd, &msgh, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN || errno == ESHUTDOWN) {
            IDK_ERR("fs", "sendmsg: peer closed (errno=%d: %s) — shutting down socket\n",
                    errno, strerror(errno));
            idk_fs_shutdown();
        } else {
            IDK_ERR("fs", "sendmsg failed: %s\n", strerror(errno));
        }
        return -1;
    }

    return 0;
}

int idk_fs_wait_ack(void) {
    if (g_sock_fd < 0) {
        errno = ENOTCONN;
        return -1;
    }
    char ack = 0;
    ssize_t n = read(g_sock_fd, &ack, 1);
    if (n <= 0) {
        if (n == 0) errno = ECONNRESET;
        return -1;
    }
    return 0;
}
