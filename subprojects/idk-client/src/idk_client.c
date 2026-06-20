/*
 * idk_client.c — Client library for sending overlay frames to idk-overlay socket
 *
 * Adapted from imgoverlay client. Bridges the imgoverlay message protocol
 * (create/update/destroy images via MSG_CREATE_IMAGE, MSG_UPDATE_IMAGE,
 * MSG_UPDATE_IMAGE_CONTENTS) into idk-overlay's wire format:
 *
 *   32-byte header + 1 fd via SCM_RIGHTS
 *
 * The imgoverlay protocol sends metadata and file descriptors separately
 * (msg_struct + writeFds), while idk-overlay sends them atomically in
 * one sendmsg() call. This module translates between the two.
 *
 * Wire mapping:
 *   imgoverlay msg_create_image.x  → idk header.stride  (overlay X)
 *   imgoverlay msg_create_image.y  → idk header.format  (overlay Y)
 *   imgoverlay msg_create_image.id → idk header.num_planes (overlay ID)
 *   imgoverlay msg_create_image.memsize → idk header.pid (pixel byte size)
 *   new field                      → idk header.reserved (visibility)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include "idk_client.h"
#include "idk_ipc.h"

/* ── Internal state ───────────────────────────────────────────────────── */

static int g_sock_fd = -1;
static char g_sock_path[IDK_IPC_SOCKNAME_MAX];

/* ── CRC32 ─────────────────────────────────────────────────────────────── */

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

/* ── Frame header builder ─────────────────────────────────────────────── */

/**
 * Build an idk-overlay frame header from an idk_client_frame.
 *
 * Maps imgoverlay-style position/visibility info into the idk wire format.
 */
static void build_frame_hdr(const idk_client_frame_t *frame,
                            uint8_t *hdr, size_t hdr_size) {
    if (hdr_size < sizeof(uint32_t) * 8) return;

    uint32_t *fields = (uint32_t *)hdr;
    fields[0] = frame->width;        /* width  */
    fields[1] = frame->height;       /* height */
    fields[2] = frame->x;            /* stride → X position */
    fields[3] = frame->y;            /* format → Y position */
    fields[4] = (uint32_t)frame->id; /* num_planes → overlay ID */
    fields[5] = frame->width * frame->height * 4; /* pid → pixel byte size */
    fields[6] = (uint32_t)frame->visible; /* reserved → visibility */
    /* fields[7] = checksum — set by send_frame */
}

/* ── SHM helpers ──────────────────────────────────────────────────────── */

/**
 * Create an anonymous SHM segment, copy data into it, and return the fd.
 * The caller must munmap and close the fd when done.
 */
static int copy_to_shm(const void *src, size_t size) {
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/idk-client-%d", (int)getpid());

    int fd = shm_open(shm_name, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        fprintf(stderr, "[idk-client] shm_open failed: %s\n", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, size) < 0) {
        fprintf(stderr, "[idk-client] ftruncate failed: %s\n", strerror(errno));
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }

    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "[idk-client] mmap failed: %s\n", strerror(errno));
        close(fd);
        shm_unlink(shm_name);
        return -1;
    }

    memcpy(map, src, size);
    munmap(map, size);

    return fd;
}

/* ── Public API ───────────────────────────────────────────────────────── */

int idk_client_init(const char *sockpath, int reuse_fd) {
    if (!sockpath) {
        errno = EINVAL;
        return -1;
    }

    /* If reusing, just store path and return */
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
            fprintf(stderr, "[idk-client] socket() failed: %s\n", strerror(errno));
            return -1;
        }

        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        memcpy(addr.sun_path, sockpath, len + 1);

        /* Retry connect() a few times — server may not be ready yet.
         * This handles the case where client-qt starts before the game
         * (and before the compositor socket is created). */
        int connect_ret = -1;
        for (int i = 0; i < 30; i++) {
            connect_ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
            if (connect_ret == 0) break;
            if (errno != ECONNREFUSED && errno != ENOENT) {
                fprintf(stderr, "[idk-client] connect(%s) failed: %s\n",
                        sockpath, strerror(errno));
                close(fd);
                return -1;
            }
            usleep(100000); /* 100ms between retries, ~3s total */
        }

        if (connect_ret < 0) {
            fprintf(stderr,
                    "[idk-client] connect(%s) failed after retries: %s\n",
                    sockpath, strerror(errno));
            close(fd);
            return -1;
        }

        g_sock_fd = fd;
        fprintf(stderr, "[idk-client] Connected to %s (fd=%d)\n", sockpath, fd);
    } else {
        g_sock_fd = reuse_fd;
        fprintf(stderr, "[idk-client] Reusing fd=%d for %s\n", reuse_fd, sockpath);
    }

    return 0;
}

int idk_client_get_fd(void) {
    return g_sock_fd;
}

void idk_client_shutdown(void) {
    if (g_sock_fd >= 0) {
        close(g_sock_fd);
        g_sock_fd = -1;
    }
}

int idk_client_send_frame(int data_fd, const idk_client_frame_t *frame) {
    if (g_sock_fd < 0) {
        errno = ENOTCONN;
        return -1;
    }
    if (!frame) {
        errno = EINVAL;
        return -1;
    }

    /* Build and send using idk-overlay's wire format */
    uint8_t hdr[32] = { 0 };
    build_frame_hdr(frame, hdr, sizeof(hdr));

    /* Calculate checksum on first 7 fields (exclude checksum field) */
    uint32_t checksum = crc32_simple(hdr, sizeof(hdr) - sizeof(uint32_t));
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

    ssize_t n = sendmsg(g_sock_fd, &msgh, 0);
    if (n < 0) {
        fprintf(stderr, "[idk-client] sendmsg failed: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr,
            "[idk-client] Frame sent: %dx%d@(%d,%d) id=%d visible=%d fd=%d\n",
            frame->width, frame->height, frame->x, frame->y,
            frame->id, frame->visible, data_fd);
    return 0;
}

int idk_client_send_pixels(const void *pixels, const idk_client_frame_t *frame) {
    if (!pixels || !frame) {
        errno = EINVAL;
        return -1;
    }

    size_t pixel_size = (size_t)frame->width * (size_t)frame->height * 4;
    int shm_fd = copy_to_shm(pixels, pixel_size);
    if (shm_fd < 0) {
        fprintf(stderr, "[idk-client] copy_to_shm failed\n");
        return -1;
    }

    int rc = idk_client_send_frame(shm_fd, frame);
    close(shm_fd);

    /*
     * Wait a brief moment for the receiver to read the frame before closing.
     * This ensures the fd transfer completes atomically — the peer must call
     * recvmsg with SCM_RIGHTS before we close, otherwise the fd is lost.
     */
    usleep(50000); /* 50ms */

    return rc;
}

/* ── DMA-BUF sender (for GPU-rendered content) ────────────────────────── */

/**
 * Send a DMA-BUF fd directly (no SHM copy).
 * Supports multi-plane DMA-BUF via frame->nfd.
 */
int idk_client_send_dma_buf(const int *dma_buf_fds, const idk_client_frame_t *frame) {
    if (g_sock_fd < 0) {
        errno = ENOTCONN;
        return -1;
    }
    if (!frame || !dma_buf_fds || (size_t)frame->nfd == 0 || (size_t)frame->nfd > 4) {
        errno = EINVAL;
        return -1;
    }

    /* Build header */
    uint8_t hdr[32] = { 0 };
    build_frame_hdr(frame, hdr, sizeof(hdr));

    /* Calculate checksum */
    uint32_t checksum = crc32_simple(hdr, sizeof(hdr) - sizeof(uint32_t));
    ((uint32_t *)hdr)[7] = checksum;

    /* Prepare I/O vector and control buffer for multiple fds */
    struct iovec iov = { .iov_base = hdr, .iov_len = sizeof(hdr) };
    char ctrl_buf[CMSG_SPACE(sizeof(int) * 4)] = { 0 };
    struct msghdr msgh = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buf,
        .msg_controllen = sizeof(ctrl_buf),
    };

    /* Write all DMA-BUF fds as ancillary data */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * frame->nfd);
    memcpy(CMSG_DATA(cmsg), dma_buf_fds, sizeof(int) * frame->nfd);
    msgh.msg_controllen = cmsg->cmsg_len;

    ssize_t n = sendmsg(g_sock_fd, &msgh, 0);
    if (n < 0) {
        fprintf(stderr, "[idk-client] sendmsg failed: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr,
            "[idk-client] DMA-BUF sent: %dx%d@(%d,%d) id=%d visible=%d nfd=%d fd0=%d\n",
            frame->width, frame->height, frame->x, frame->y,
            frame->id, frame->visible, frame->nfd, dma_buf_fds[0]);
    return 0;
}
