/*
 * receive.c — Receive dmabuf frame from external process (webview),
 *             render overlay onto it.
 *
 * Injected process calls receive_frame() to get dmabuf fd from webview
 * (or any external process). Then render overlay (text, UI, clock)
 * onto the dmabuf buffer.
 *
 * The caller is responsible for displaying the result:
 *   - Send back to external process via socket (for display)
 *   - Or render into target process framebuffer directly
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <poll.h>

#include "public/idk_ipc.h"
#include "core/log.h"

/* ── Frame header ─────────────────────────────────────────────────────── */

struct frame_hdr {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t num_planes;
    uint32_t pid;
    uint32_t reserved;
    uint32_t checksum;
};

/* ── CRC32 (same as idk_ipc.c) ────────────────────────────────────────── */

static uint32_t __attribute__((unused)) crc32_simple(const void *data, size_t len) {
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

/* ── Receive frame from socket ────────────────────────────────────────── */

/**
 * Receive one frame (dmabuf or SHM fd) from the connected socket.
 *
 * @param sock_fd    Connected socket fd.
 * @param hdr        Output: frame header.
 * @param out_fd     Output: received dmabuf/SHM fd (caller must close).
 * @return           0 on success, -1 on EOF/error.
 */
int receive_frame(int sock_fd, struct frame_hdr *hdr, int *out_fd) {
    char buf[64];
    char ctrl_buf[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msgh = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buf,
        .msg_controllen = sizeof(ctrl_buf),
    };

    struct pollfd pfd = { .fd = sock_fd, .events = POLLIN };
    if (poll(&pfd, 1, 2000) <= 0 || !(pfd.revents & POLLIN)) {
        return -1;
    }

    ssize_t n = recvmsg(sock_fd, &msgh, 0);
    if (n <= 0) {
        if (n == 0) return -1; /* peer closed */
        IDK_ERR("render", "recvmsg failed: %s\n", strerror(errno));
        return -1;
    }

    *out_fd = -1;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh); cmsg;
         cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
            break;
        }
    }

    if (*out_fd < 0) {
        IDK_ERR("render", "No fd received\n");
        return -1;
    }

    memcpy(hdr, buf, sizeof(*hdr));
    return 0;
}

/* ── Overlay rendering helpers ────────────────────────────────────────── */

/* Simple 5x7 font for A-Z (5 chars × 5 rows × 5 cols) */
static const uint8_t font_AZ[][5] = {
    ['A' - 'A'] = {0x10, 0x38, 0x7e, 0x38, 0x38},
    ['B' - 'A'] = {0x7e, 0x42, 0x42, 0x42, 0x7e},
    ['C' - 'A'] = {0x3c, 0x42, 0x42, 0x42, 0x3c},
    ['D' - 'A'] = {0x7e, 0x42, 0x42, 0x42, 0x7e},
    ['E' - 'A'] = {0x7e, 0x42, 0x7e, 0x42, 0x7e},
    ['F' - 'A'] = {0x7e, 0x42, 0x7e, 0x42, 0x42},
    ['G' - 'A'] = {0x3c, 0x42, 0x4a, 0x42, 0x3e},
    ['H' - 'A'] = {0x7e, 0x38, 0x38, 0x38, 0x7e},
    ['I' - 'A'] = {0x42, 0x42, 0x7e, 0x42, 0x42},
    ['J' - 'A'] = {0x1e, 0x02, 0x02, 0x42, 0x7e},
    ['K' - 'A'] = {0x7e, 0x42, 0x6c, 0x6c, 0x44},
    ['L' - 'A'] = {0x7e, 0x42, 0x42, 0x42, 0x42},
    ['M' - 'A'] = {0x7e, 0x7e, 0x7e, 0x6c, 0x6c},
    ['N' - 'A'] = {0x7e, 0x7e, 0x6c, 0x38, 0x1e},
    ['O' - 'A'] = {0x3c, 0x42, 0x42, 0x42, 0x3c},
    ['P' - 'A'] = {0x7e, 0x42, 0x7e, 0x42, 0x42},
    ['Q' - 'A'] = {0x3c, 0x42, 0x42, 0x42, 0x3c},
    ['R' - 'A'] = {0x7e, 0x42, 0x7e, 0x42, 0x7e},
    ['S' - 'A'] = {0x3e, 0x40, 0x3e, 0x04, 0x7a},
    ['T' - 'A'] = {0x7e, 0x02, 0x02, 0x02, 0x02},
    ['U' - 'A'] = {0x7e, 0x02, 0x02, 0x02, 0x7e},
    ['V' - 'A'] = {0x7e, 0x02, 0x02, 0x02, 0x02},
    ['W' - 'A'] = {0x7e, 0x1c, 0x1c, 0x1c, 0x7e},
    ['X' - 'A'] = {0x74, 0x38, 0x10, 0x38, 0x74},
    ['Y' - 'A'] = {0x7e, 0x02, 0x02, 0x02, 0x02},
    ['Z' - 'A'] = {0x7e, 0x04, 0x08, 0x10, 0x7e},
};

static const size_t font_AZ_rows = sizeof(font_AZ) / sizeof(font_AZ[0]);

/**
 * Render ASCII text onto ABGR8888 framebuffer.
 *
 * @param pixels   Pixel buffer (mmap'd dmabuf or SHM).
 * @param stride   Number of bytes per row (width * 4 for ABGR8888).
 * @param width    Framebuffer width in pixels.
 * @param height   Framebuffer height in pixels.
 * @param text     Null-terminated string to render.
 * @param x        Top-left X coordinate (in pixels).
 * @param y        Top-left Y coordinate (in pixels).
 * @param r        Red component (0-255).
 * @param g        Green component (0-255).
 * @param b        Blue component (0-255).
 * @param a        Alpha component (0-255).
 */
void render_text(char *pixels, size_t stride,
                 uint32_t width, uint32_t height,
                 const char *text, int x, int y,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (; *text; text++) {
        if (*text < 'A' || *text > 'Z') continue;

        int c = *text - 'A';
        if (c < 0 || (size_t)c >= font_AZ_rows) {
            x += 7; /* advance for unknown char */
            continue;
        }

        const uint8_t *glyph = font_AZ[c];
        for (int row = 0; row < 5; row++) {
            for (int bit = 0; bit < 5; bit++) {
                if (glyph[row] & (1 << bit)) {
                    int px = x + row;
                    int py = y + bit;
                    if (px >= 0 && px < (int)width &&
                        py >= 0 && py < (int)height) {
                        uint8_t *dst = &pixels[py * stride + px * 4];
                        dst[0] = b; dst[1] = g;
                        dst[2] = r; dst[3] = a;
                    }
                }
            }
        }
        x += 7;
    }
}

/**
 * Render a clock onto ABGR8888 framebuffer.
 *
 * @param pixels   Pixel buffer (mmap'd dmabuf or SHM).
 * @param stride   Number of bytes per row (width * 4 for ABGR8888).
 * @param width    Framebuffer width in pixels.
 * @param height   Framebuffer height in pixels.
 * @param x        Top-left X coordinate.
 * @param y        Top-left Y coordinate.
 * @param r,g,b,a  Text color.
 */
void render_clock(char *pixels, size_t stride,
                  uint32_t width, uint32_t height,
                  int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    render_text(pixels, stride, width, height, buf, x, y, r, g, b, a);
}

/**
 * Render a complete overlay onto ABGR8888 framebuffer.
 *
 * @param pixels   Pixel buffer (mmap'd dmabuf or SHM).
 * @param stride   Number of bytes per row (width * 4 for ABGR8888).
 * @param width    Framebuffer width in pixels.
 * @param height   Framebuffer height in pixels.
 */
void render_overlay(char *pixels, size_t stride,
                    uint32_t width, uint32_t height) {
    /* Render title */
    render_text(pixels, stride, width, height,
                "IDK OVERLAY", 10, 10, 255, 255, 255, 255);

    /* Render clock */
    render_clock(pixels, stride, width, height,
                 10, 25, 200, 200, 255, 255); /* Cyan-ish */
}
