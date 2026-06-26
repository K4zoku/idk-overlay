/*
 * render.c — idk-render: receives dmabuf/SHM frames and reads pixels
 *
 * Supports two frame sources:
 *   1. Injected library (GL/VK hooks) — full-screen framebuffer frames
 *   2. External client (idk-framesource) — overlay frames with position
 *
 * Usage:
 *   idk-render [--socket /tmp/idk-overlay-XXXX] [--output frame.pnm]
 *
 * Runs in a separate process from the target. Listens on a Unix domain
 * socket, receives dmabuf fds (SCM_RIGHTS), imports them into Vulkan
 * or reads from SHM, and optionally writes pixels to a file.
 *
 * Overlay frame header mapping (from idk_client):
 *   width     → width
 *   height    → height
 *   stride    → overlay X position
 *   format    → overlay Y position
 *   num_planes → overlay ID
 *   pid       → pixel byte size
 *   reserved  → visibility flag
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <poll.h>

#include "public/idk_ipc.h"
#include "core/log.h"

/* ── Overlay management ──────────────────────────────────────────────── */

#define RENDER_MAX_OVERLAYS 16

typedef struct render_overlay {
    uint8_t  id;             /* Overlay ID (1-based) */
    uint32_t x, y;           /* Position on screen */
    uint32_t width, height;  /* Size in pixels */
    uint32_t pixel_size;     /* width * height * 4 */
    uint8_t  visible;        /* 0 = hidden, 1 = visible */
    int      fd;             /* Current frame fd (mmap'd) */
    void    *pixels;         /* mmap'd pixel pointer */
    uint8_t  source;         /* 0 = injected, 1 = external client */
} render_overlay_t;

static render_overlay_t g_overlays[RENDER_MAX_OVERLAYS];
static int g_num_overlays = 0;

/* ── Global state (shared with main) ─────────────────────────────────── */

static const char *g_sockpath = "/tmp/idk-overlay";
static int g_max_frames = 0;
static int g_frame_count = 0;
static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int create_server_socket(const char *sockpath) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        IDK_ERR("render", "socket() failed: %s\n", strerror(errno));
        return -1;
    }

    unlink(sockpath);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t len = strlen(sockpath);
    if (len >= sizeof(addr.sun_path)) {
        IDK_ERR("render", "socket path too long\n");
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, sockpath, len + 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        IDK_ERR("render", "bind() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        IDK_ERR("render", "listen() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    IDK_LOG("render", "Listening on %s\n", sockpath);
    return fd;
}

static int accept_client(int server_fd) {
    struct sockaddr_un addr = { 0 };
    socklen_t addr_len = sizeof(addr);
    return accept(server_fd, (struct sockaddr *)&addr, &addr_len);
}

static int find_or_create_overlay(uint8_t id) {
    for (int i = 0; i < g_num_overlays; i++) {
        if (g_overlays[i].id == id) return i;
    }
    if (g_num_overlays >= RENDER_MAX_OVERLAYS) {
        IDK_ERR("render", "Max overlays (%d) reached\n", RENDER_MAX_OVERLAYS);
        return -1;
    }
    int idx = g_num_overlays++;
    memset(&g_overlays[idx], 0, sizeof(render_overlay_t));
    g_overlays[idx].id = id;
    g_overlays[idx].fd = -1;
    g_overlays[idx].source = 1; /* external client */
    return idx;
}

static void update_overlay(render_overlay_t *ov, const void *info, size_t info_len,
                           int fd) {
    (void)info_len;
    const idk_frame_header_t *hdr = (const idk_frame_header_t *)info;

    ov->width      = hdr->width;
    ov->height     = hdr->height;
    ov->x          = 0;  /* position no longer in header */
    ov->y          = 0;
    ov->pixel_size = hdr->width * hdr->height * 4;
    ov->visible    = (hdr->flags & IDK_FRAME_FLAG_VISIBLE) ? 1 : 0;

    if (ov->fd >= 0) {
        if (ov->pixels) {
            munmap(ov->pixels, ov->pixel_size);
            ov->pixels = NULL;
        }
        close(ov->fd);
    }

    ov->fd = fd;
    if (ov->pixel_size > 0) {
        ov->pixels = mmap(NULL, ov->pixel_size, PROT_READ, MAP_SHARED, fd, 0);
        if (ov->pixels == MAP_FAILED) {
            ov->pixels = NULL;
            IDK_ERR("render", "mmap overlay failed: %s\n", strerror(errno));
            return;
        }
    }

    IDK_LOG("render",
            "Overlay: %dx%d vis=%d src=%s\n",
            ov->width, ov->height, ov->visible,
            ov->source ? "client" : "injected");
}

static void process_frame(const void *info, size_t info_len, int fd) {
    (void)info_len;
    g_frame_count++;

    const idk_frame_header_t *hdr = (const idk_frame_header_t *)info;

    /* In the new protocol, all frames go through the same path.
     * The old overlay_id concept (from num_planes field) is gone. */
    IDK_LOG("render",
            "Frame #%d: %dx%d stride=%u flags=0x%02x fd=%d\n",
            g_frame_count, hdr->width, hdr->height, hdr->stride,
            hdr->flags, fd);

    /* Treat as full-screen frame (injected library) */
    {
        size_t pixel_size = (size_t)hdr->width * (size_t)hdr->height * 4;
        uint8_t *pixels = mmap(NULL, pixel_size, PROT_READ, MAP_SHARED, fd, 0);
        if (pixels == MAP_FAILED) {
            IDK_ERR("render", "mmap SHM failed: %s\n", strerror(errno));
            close(fd);
            return;
        }

        IDK_LOG("render",
                "SHM frame loaded: %u bytes at %p\n",
                (unsigned)pixel_size, (void *)pixels);

        munmap(pixels, pixel_size);
    }
    close(fd);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --socket PATH   Unix socket path (default: /tmp/idk-overlay)\n"
        "  --max N         Receive N frames then exit (0 = infinite)\n"
        "  -h, --help      Show this help\n",
        prog);
}

int main(int argc, char **argv) {
    static struct option long_opts[] = {
        { "socket", required_argument, NULL, 's' },
        { "max",    required_argument, NULL, 'm' },
        { "help",   no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:m:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': g_sockpath = optarg; break;
        case 'm': g_max_frames = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    struct sigaction sa = { .sa_handler = signal_handler };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    IDK_LOG("render", "Starting (sock=%s max=%d)\n", g_sockpath, g_max_frames);

    int server_fd = create_server_socket(g_sockpath);
    if (server_fd < 0) {
        return 1;
    }

    int client_fd = -1;

    while (g_running) {
        if (g_max_frames > 0 && g_frame_count >= g_max_frames) {
            break;
        }

        /* If no client connected, wait for one */
        if (client_fd < 0) {
            struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
            if (poll(&pfd, 1, 1000) <= 0) continue;

            int new_fd = accept_client(server_fd);
            if (new_fd >= 0) {
                client_fd = new_fd;
                IDK_LOG("render", "Client connected\n");
            }
            continue;
        }

        /* Receive frames from the connected client */
        idk_frame_header_t hdr;
        int fd = -1;
        int rc = idk_ipc_recv_frame(client_fd, &hdr, &fd);

        if (rc < 0) {
            /* Client disconnected or timeout */
            IDK_LOG("render", "Client disconnected\n");
            close(client_fd);
            client_fd = -1;
            continue;
        }

        process_frame(&hdr, sizeof(hdr), fd);
    }

    if (client_fd >= 0) close(client_fd);
    close(server_fd);
    unlink(g_sockpath);

    IDK_LOG("render", "Shutdown. Total frames: %d\n", g_frame_count);
    return 0;
}
