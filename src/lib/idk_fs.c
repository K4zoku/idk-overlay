/* idk_fs.c — Frame sender library (client side of compositor).
 *
 * Uses idk_transport under the hood. Phase 1: socket backend.
 * Phase 2: SHM backend (transparent swap).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "public/idk_fs.h"
#include "public/idk_ipc.h"
#include "core/transport.h"
#include "core/log.h"

/* Internal state */

static idk_transport_t g_tp;

/* SHM helper — copy pixels into a memfd for sending. */
static int copy_to_shm(const void *src, size_t size) {
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/idk-framesource-%d", (int)getpid());

    /* Use 0600 (owner read/write only) instead of 0666 (world r/w).
     * The predictable name + 0666 meant any local user could open the
     * SHM and read/modify overlay pixel data. With 0600, only the
     * owning user can access it. The umask may further restrict but
     * never widen these perms. */
    int fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
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

int idk_fs_init(const char *sockpath) {
    if (!sockpath) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(sockpath);
    if (len >= IDK_TP_PATH_MAX) {
        errno = ERANGE;
        return -1;
    }
    return idk_tp_init(&g_tp, IDK_TP_PRODUCER, sockpath);
}

bool idk_fs_is_connected(void) {
    return g_tp.ready;
}

void idk_fs_shutdown(void) {
    idk_tp_destroy(&g_tp);
}

/* Internal: sendmsg with frame header + N fds. */
static int send_frame_msg(const idk_frame_header_t *hdr, const int *fds, int nfd) {
    if (!g_tp.ready || !hdr || !fds || nfd < 1 || nfd > 4) {
        errno = EINVAL;
        return -1;
    }

    return idk_tp_send(&g_tp, hdr, fds, nfd);
}

int idk_fs_send_frame(int data_fd, const idk_frame_header_t *hdr) {
    if (!hdr) {
        errno = EINVAL;
        return -1;
    }
    int rc = send_frame_msg(hdr, &data_fd, 1);
    if (rc == 0) {
        IDK_LOG("fs",
                "Frame sent: %dx%d stride=%u flags=0x%02x fd=%d\n",
                hdr->width, hdr->height, hdr->stride, hdr->flags, data_fd);
    }
    return rc;
}

int idk_fs_send_pixels(const void *pixels, const idk_frame_header_t *hdr) {
    if (!pixels || !hdr) {
        errno = EINVAL;
        return -1;
    }

    idk_frame_header_t f = *hdr;
    f.flags = IDK_FRAME_FLAG_VISIBLE;
    f.stride = 0;

    /* Validate dimensions to prevent integer overflow in pixel_size.
     * If width and height are both near UINT32_MAX, the multiplication
     * overflows size_t on 64-bit (max ~2^64) only at extreme values,
     * but on 32-bit it wraps much earlier. Cap at 65536x65536 (4 GiB
     * at 4 bytes/pixel) which is far beyond any realistic overlay
     * resolution. A malicious/corrupted header with huge dimensions
     * would otherwise produce a tiny pixel_size → undersized SHM →
     * truncated memcpy. */
    if (f.width == 0 || f.height == 0 ||
        f.width > 65536 || f.height > 65536) {
        IDK_ERR("fs", "idk_fs_send_pixels: invalid dimensions %ux%u\n",
                (unsigned)f.width, (unsigned)f.height);
        errno = EINVAL;
        return -1;
    }

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

int idk_fs_send_dma_buf(const int *dma_buf_fds, const idk_frame_header_t *hdr) {
    if (!hdr || !dma_buf_fds || hdr->nfd == 0 || hdr->nfd > 4) {
        errno = EINVAL;
        return -1;
    }

    idk_frame_header_t f = *hdr;
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

int idk_fs_wait_ack(idk_ack_msg_t *ack, int timeout_ms) {
    if (!g_tp.ready) {
        errno = ENOTCONN;
        return -1;
    }
    return idk_tp_wait_ack(&g_tp, ack, timeout_ms);
}

int idk_fs_recv_request(idk_request_msg_t *req, int timeout_ms) {
    if (!g_tp.ready) {
        errno = ENOTCONN;
        return -1;
    }
    return idk_tp_recv_request(&g_tp, req, timeout_ms);
}
