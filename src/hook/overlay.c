#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "hook/syringe_hook.h"
#include "hook/overlay.h"
#include "public/idk_ipc.h"
#include "hook/vulkan.h"
#include "hook/egl.h"
#include "hook/glx.h"
#include "hook/wayland_input.h"
#include "core/log.h"

static char g_socket_path[PATH_MAX];
static int g_ipc_fd = -1;
static int g_enable_vk = 0;
static int g_enable_gl = 0;
static int g_initialized = 0;
static int g_wl_input_tried = 0;  /* avoid retrying wayland input hook every swap */
static int g_hooks_installed = 0; /* set by background thread when any hook succeeds */

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

/* Background thread: poll for GL/EGL/Vulkan libraries */
static void *hook_install_thread(void *arg) {
    (void)arg;

    usleep(50000);
    for (int i = 0; i < 150 && !g_wl_input_tried; i++) {
        void *h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libwayland-client.so", RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            dlclose(h);
            idk_overlay_try_install_wayland_input();
            break;
        }
        usleep(10000);
    }

    int egl_done = 0;
    int vk_done = 0;

    for (int i = 0; i < 150; i++) {
        if (g_enable_gl && !egl_done) {
            void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
            if (!h) h = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
            if (h) {
                dlclose(h);
                int r = idk_egl_init();
                if (r == 0) {
                    egl_done = 1;
                    g_hooks_installed = 1;
                } else {
                    DBG("EGL hook install failed, will retry");
                }
            }
        }

        if (g_enable_vk && !vk_done) {
            void *h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
            if (!h) h = dlopen("libvulkan.so", RTLD_NOW | RTLD_NOLOAD);
            if (h) {
                dlclose(h);
                int r = idk_vulkan_init(g_ipc_fd, g_socket_path);
                if (r == 0) {
                    vk_done = 1;
                    g_hooks_installed = 1;
                } else {
                    DBG("Vulkan hook install failed, will retry");
                }
            }
        }

        if (egl_done && vk_done) break;
        if (!g_enable_gl) egl_done = 1;
        if (!g_enable_vk) vk_done = 1;
        if (egl_done && vk_done) break;

        usleep(200000);
    }

    if (!g_hooks_installed) {
        DBG("no graphics hooks installed after 30s — "
            "game may not use EGL or Vulkan, or libraries not detected");
    }

    return NULL;
}

int idk_overlay_init(const char *socket_path, int enable_vk, int enable_gl) {
    if (g_initialized) return 0;
    g_initialized = 1;
    g_enable_vk = enable_vk;
    g_enable_gl = enable_gl;

    if (socket_path)
        snprintf(g_socket_path, sizeof(g_socket_path), "%s", socket_path);
    else
        snprintf(g_socket_path, sizeof(g_socket_path), "/tmp/idk-overlay-%d", getpid());



    g_ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ipc_fd < 0) {
        DBG("GLX IPC socket creation failed: %s (EGL path unaffected)", strerror(errno));
        g_ipc_fd = -1;
    } else {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", g_socket_path);
        if (connect(g_ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            /* ENOENT/ECONNREFUSED is normal at startup — idk-render not running.
             * GLX path will reconnect when needed; EGL path ignores this fd. */
            DBG("GLX IPC not connected (idk-render not running) — EGL path unaffected");
            close(g_ipc_fd);
            g_ipc_fd = -1;
        }
    }


    void *wlh = dlopen("libwayland-client.so.0", RTLD_NOW);
    if (!wlh) wlh = dlopen("libwayland-client.so", RTLD_NOW);
    if (wlh) {
        idk_overlay_try_install_wayland_input();
        dlclose(wlh);
    }


    pthread_t t;
    if (pthread_create(&t, NULL, hook_install_thread, NULL) == 0) {
        pthread_detach(t);
    } else {
        DBG("FATAL: pthread_create failed — hooks will not be installed");
    }

    return 0;
}

void idk_overlay_shutdown(void) {
    if (g_enable_vk) idk_vulkan_shutdown();
    if (g_enable_gl) {
        idk_glx_shutdown();
        idk_egl_shutdown();
    }
    idk_wayland_input_shutdown();

    if (g_ipc_fd >= 0) {
        close(g_ipc_fd);
        g_ipc_fd = -1;
    }
}

int idk_overlay_try_install_wayland_input(void) {
    if (g_wl_input_tried) return 0;
    g_wl_input_tried = 1;
    int r = idk_wayland_input_init();
    return r;
}

/* AppImage guard: g_initialized prevents double-init */
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
