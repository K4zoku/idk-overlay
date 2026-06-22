/*
 * overlay.c — Main entry point for the injectable library
 *
 * This file is compiled into libidk-overlay.so. It is injected into the
 * target process via syringe_inject(). The __attribute__((constructor))
 * automatically initializes hooks and the IPC pipe.
 *
 * Architecture (Wayland EGL):
 *   Webview (offscreen render) → export dmabuf → socket → injected process
 *   Injected process:
 *     - Receives overlay frames from webview via Unix socket
 *     - Converts to GL texture via EGL DMA-BUF import
 *     - Renders fullscreen quad on top of game's EGL framebuffer
 *     - Calls original swap → game + overlay are presented together
 *
 * This is the same approach used by Steam/Discord overlays on Wayland.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "hook/syringe_hook.h"
#include "hook/overlay.h"
#include "public/idk_ipc.h"
#include "hook/vulkan.h"
#include "gl/egl.h"
#include "core/log.h"

/* ── Global state ─────────────────────────────────────────────────────── */

static char g_socket_path[PATH_MAX];
static int g_ipc_fd = -1;
static int g_enable_vk = 0;
static int g_enable_gl = 0;
static int g_initialized = 0;

/* Debug log file — bypass stderr (which AppImage may redirect). */
static FILE *g_dbg = NULL;
static void dbg_init(void) {
    if (g_dbg) return;
    g_dbg = fopen("/tmp/idk-debug.log", "a");
    if (g_dbg) {
        setvbuf(g_dbg, NULL, _IONBF, 0);
        fprintf(g_dbg, "\n=== idk-overlay debug log (PID %d) ===\n", getpid());
    }
}
#define DBG(fmt, ...) do { \
    if (g_dbg) fprintf(g_dbg, "[idk-overlay] " fmt "\n", ##__VA_ARGS__); \
    IDK_LOG("overlay", fmt "\n", ##__VA_ARGS__); \
} while(0)


/* ── Public API ───────────────────────────────────────────────────────── */

int idk_overlay_init(const char *socket_path, int enable_vk, int enable_gl) {
    /* Guard against double init (e.g., AppImage double mount) */
    if (g_initialized) {
        DBG("Already initialized (duplicate load ignored)");
        return 0;
    }
    g_initialized = 1;
    g_enable_vk = enable_vk;
    g_enable_gl = enable_gl;
    if (socket_path) {
        snprintf(g_socket_path, sizeof(g_socket_path), "%s", socket_path);
    } else {
        snprintf(g_socket_path, sizeof(g_socket_path), "/tmp/idk-overlay");
    }

    DBG("PID=%d sock=%s vk=%d gl=%d",
            getpid(), g_socket_path, enable_vk, enable_gl);

    /* Connect to idk-render (for framebuffer capture, optional) */
    g_ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ipc_fd < 0) {
        DBG("IPC socket failed: %s", strerror(errno));
        g_ipc_fd = -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", g_socket_path);

    if (connect(g_ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        DBG("IPC connect failed: %s (frame capture disabled)", strerror(errno));
        close(g_ipc_fd);
        g_ipc_fd = -1;
    } else {
        DBG("Connected to idk-render for framebuffer capture");
    }

    /* Initialize Vulkan hooks (try regardless of IPC state) */
    if (enable_vk) {
        DBG("Initializing Vulkan hooks...");
        idk_vulkan_init(g_ipc_fd >= 0 ? g_ipc_fd : -1, g_socket_path);
    }

    /* Initialize EGL hooks (for OpenGL/Wayland apps like osu-lazer).
     * Uses syringe_hook_install_addr to bypass GOT — works even when
     * the target dlopen'd libEGL.so.1 itself (SDL pattern). */
    if (enable_gl) {
        DBG("Initializing EGL hooks...");
        if (idk_egl_init() != 0) {
            DBG("EGL hook init failed (not an EGL app?)");
        }
    }

    return 0;
}

void idk_overlay_shutdown(void) {
    if (g_enable_vk) idk_vulkan_shutdown();
    if (g_enable_gl) idk_egl_shutdown();

    if (g_ipc_fd >= 0) {
        close(g_ipc_fd);
        g_ipc_fd = -1;
    }
}

/* ── AppImage fork/exec handling ───────────────────────────────────────── */
/*
 * AppImage wrapper forks then execs the child. Both the wrapper's and
 * the child's constructors run in the same PID (fork+exec).
 *
 * flock() can't work across exec — it's process-wide, not per-fd.
 * O_EXCL file markers can't work — the wrapper's marker survives exec,
 * blocking the child.
 *
 * Solution: Let all instances call idk_overlay_init(). The g_initialized
 * guard in idk_overlay_init() prevents double-init WITHIN the same
 * process. The compositor's socket-binding (in compositor.c) acts as
 * the real single-instance gate — only the first process to bind the
 * Unix socket becomes the "real" instance; others detect the alive
 * socket and exit().
 */

__attribute__((constructor))
static void on_load(void) {
    dbg_init();
    DBG("Initializing...");
    const char *env_vk = getenv("IDK_VK");
    const char *env_gl = getenv("IDK_GL");
    const char *env_path = getenv("IDK_SOCKET");
    idk_overlay_init(env_path ? env_path : NULL,
                     env_vk ? atoi(env_vk) : 0,
                     env_gl ? atoi(env_gl) : 1);
}

__attribute__((destructor))
static void on_unload(void) {
    idk_overlay_shutdown();
}
