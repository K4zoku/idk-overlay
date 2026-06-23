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
#include "hook/egl.h"
#include "hook/glx.h"
#include "core/log.h"

static char g_socket_path[PATH_MAX];
static int g_ipc_fd = -1;
static int g_enable_vk = 0;
static int g_enable_gl = 0;
static int g_initialized = 0;

static FILE *g_dbg = NULL;
static void dbg_init(void) {
    if (g_dbg) return;
    g_dbg = fopen("/tmp/idk-debug.log", "a");
    if (g_dbg) {
        setvbuf(g_dbg, NULL, _IONBF, 0);
        fprintf(g_dbg, "\n=== idk-overlay (PID %d) ===\n", getpid());
    }
}
#define DBG(fmt, ...) do { \
    if (g_dbg) fprintf(g_dbg, "[idk-overlay] " fmt "\n", ##__VA_ARGS__); \
    IDK_LOG("overlay", fmt "\n", ##__VA_ARGS__); \
} while(0)

int idk_overlay_init(const char *socket_path, int enable_vk, int enable_gl) {
    if (g_initialized) return 0;
    g_initialized = 1;
    g_enable_vk = enable_vk;
    g_enable_gl = enable_gl;

    if (socket_path)
        snprintf(g_socket_path, sizeof(g_socket_path), "%s", socket_path);
    else
        snprintf(g_socket_path, sizeof(g_socket_path), "/tmp/idk-overlay-%d", getpid());

    DBG("PID=%d sock=%s vk=%d gl=%d", getpid(), g_socket_path, enable_vk, enable_gl);

    /* Socket connection to idk-render (optional, for frame capture) */
    g_ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ipc_fd < 0) {
        DBG("IPC socket failed: %s", strerror(errno));
        g_ipc_fd = -1;
    } else {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", g_socket_path);
        if (connect(g_ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            DBG("IPC connect failed: %s", strerror(errno));
            close(g_ipc_fd);
            g_ipc_fd = -1;
        }
    }

    /* Defer hook installation to first swap call.
     * Installing hooks in the constructor is dangerous:
     * - elfhacks parses ELF headers before the linker is fully ready
     * - syringe_hook_install_addr patches code that may not be resolved yet
     * - GL/EGL/Vulkan libraries may not be loaded yet
     * The hook functions (hook_eglSwapBuffers, hook_glXSwapBuffers, etc.)
     * have a retry mechanism that installs on first call when ready. */
    DBG("hook installation deferred to first swap call");

    return 0;
}

void idk_overlay_shutdown(void) {
    if (g_enable_vk) idk_vulkan_shutdown();
    if (g_enable_gl) {
        idk_glx_shutdown();
        idk_egl_shutdown();
    }

    if (g_ipc_fd >= 0) {
        close(g_ipc_fd);
        g_ipc_fd = -1;
    }
}

/*
 * AppImage fork+exec: both wrapper and child hit this constructor.
 * g_initialized prevents double-init within the same process.
 * The compositor's socket bind acts as the cross-process gate.
 */
__attribute__((constructor))
static void on_load(void) {
    dbg_init();
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
