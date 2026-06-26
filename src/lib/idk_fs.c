/* idk_fs.c — Frame sender library (client side of compositor socket).
 *
 * Builds the 24-byte wire header (idk_frame_header_t) from idk_fs_frame_t
 * and sends it with the fd via SCM_RIGHTS. See include/public/idk_ipc.h
 * for the wire format.
 */

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

/* Build the 24-byte wire header from the client-facing frame struct. */
static void build_frame_hdr(const idk_fs_frame_t *frame,
                            idk_frame_header_t *hdr) {
    hdr->modifier = frame->modifier;
    hdr->width    = frame->width;
    hdr->height   = frame->height;
    hdr->stride   = frame->stride;
    hdr->fourcc   = frame->fourcc;
    hdr->flags    = frame->flags;
    hdr->_pad[0]  = 0;
    hdr->_pad[1]  = 0;
    hdr->_pad[2]  = 0;
}

/* SHM helper — copy pixels into a memfd for sending. */
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

/* Internal: sendmsg with frame header + N fds.
 * nfd must be 1..4. */
static int send_frame_msg(const idk_fs_frame_t *frame, const int *fds, int nfd) {
    if (g_sock_fd < 0) {
        errno = ENOTCONN;
        return -1;
    }
    if (!frame || !fds || nfd < 1 || nfd > 4) {
        errno = EINVAL;
        return -1;
    }

    idk_frame_header_t hdr;
    build_frame_hdr(frame, &hdr);

    struct iovec iov = { .iov_base = &hdr, .iov_len = sizeof(hdr) };
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
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfd);
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

int idk_fs_send_frame(int data_fd, const idk_fs_frame_t *frame) {
    if (!frame) {
        errno = EINVAL;
        return -1;
    }
    int rc = send_frame_msg(frame, &data_fd, 1);
    if (rc == 0) {
        IDK_LOG("fs",
                "Frame sent: %dx%d stride=%u flags=0x%02x fd=%d\n",
                frame->width, frame->height, frame->stride, frame->flags, data_fd);
    }
    return rc;
}

int idk_fs_send_pixels(const void *pixels, const idk_fs_frame_t *frame) {
    if (!pixels || !frame) {
        errno = EINVAL;
        return -1;
    }

    /* SHM frames always have these flags (visible, not dmabuf). */
    idk_fs_frame_t f = *frame;
    f.flags = IDK_FRAME_FLAG_VISIBLE;  /* clears DMABUF bit */
    f.stride = 0;                       /* SHM has no stride */

    size_t pixel_size = (size_t)f.width * (size_t)f.height * 4;
    int shm_fd = copy_to_shm(pixels, pixel_size);
    if (shm_fd < 0) {
        IDK_ERR("fs", "copy_to_shm failed\n");
        return -1;
    }

    int rc = idk_fs_send_frame(shm_fd, &f);
    close(shm_fd);

    usleep(50000);
    return rc;
}

int idk_fs_send_dma_buf(const int *dma_buf_fds, const idk_fs_frame_t *frame) {
    if (!frame || !dma_buf_fds || frame->nfd == 0 || frame->nfd > 4) {
        errno = EINVAL;
        return -1;
    }

    /* DMABUF frames set the DMABUF flag (keep visible bit as-is). */
    idk_fs_frame_t f = *frame;
    f.flags = (f.flags & ~IDK_FRAME_FLAG_DMABUF) | IDK_FRAME_FLAG_DMABUF;

    int rc = send_frame_msg(&f, dma_buf_fds, f.nfd);
    if (rc == 0) {
        IDK_LOG("fs",
                "DMABUF sent: %dx%d stride=%u flags=0x%02x mod=0x%llx fd=%d\n",
                f.width, f.height, f.stride, f.flags,
                (unsigned long long)f.modifier, dma_buf_fds[0]);
    }
    return rc;
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
