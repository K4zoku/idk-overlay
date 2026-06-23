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

/*
 * Background thread: poll for GL/EGL/Vulkan libraries and install hooks
 * when they're loaded by the game.
 *
 * This replaces the old "defer to first swap call" approach, which was
 * broken: the hook functions can only fire if the hook is ALREADY installed
 * (GOT/PLT patched), so a "first swap retry" that never fires is useless
 * (chicken-and-egg — hook_eglSwapBuffers is never called because the GOT
 * still points at the real eglSwapBuffers).
 *
 * The background thread starts after the constructor returns (so the linker
 * is fully ready — avoids the original constructor crash from elfhacks +
 * code patching before the linker was initialized). It polls every 200ms
 * for up to 30 seconds, using RTLD_NOLOAD to detect when the game has
 * loaded libEGL/libVulkan (we do NOT force-load them — the game should
 * load them itself; force-loading was the original constructor crash cause).
 */
static void *hook_install_thread(void *arg) {
    (void)arg;

    /* Short initial delay — let the constructor fully return and the game's
     * main() start executing. */
    usleep(50000);  /* 50ms */

    /* Install wayland input hook FIRST — games (SDL2, GLFW) register
     * wl_keyboard/wl_pointer listeners during SDL_Init, which happens
     * BEFORE libEGL is loaded. If we wait for libEGL, we miss the
     * listener registration and can't intercept input.
     *
     * libwayland-client.so.0 is loaded very early (by SDL2 during
     * display init), so polling for it first catches the registration
     * window. The EGL/Vulkan hooks are installed separately below. */
    for (int i = 0; i < 150 && !g_wl_input_tried; i++) {
        void *h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libwayland-client.so", RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            dlclose(h);
            DBG("libwayland-client detected — installing wayland input hook immediately");
            idk_overlay_try_install_wayland_input();
            break;
        }
        usleep(10000);  /* 10ms — poll fast to catch early registration */
    }

    int egl_done = 0;
    int vk_done = 0;

    for (int i = 0; i < 150; i++) {  /* 150 × 200ms = 30s */
        if (g_enable_gl && !egl_done) {
            void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
            if (!h) h = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
            if (h) {
                dlclose(h);
                DBG("libEGL detected — installing EGL hook");
                int r = idk_egl_init();
                if (r == 0) {
                    egl_done = 1;
                    g_hooks_installed = 1;
                    DBG("EGL hook installed successfully");
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
                DBG("libvulkan detected — installing Vulkan hook");
                int r = idk_vulkan_init(g_ipc_fd, g_socket_path);
                if (r == 0) {
                    vk_done = 1;
                    g_hooks_installed = 1;
                    DBG("Vulkan hook installed successfully");
                } else {
                    DBG("Vulkan hook install failed, will retry");
                }
            }
        }

        /* If both enabled hooks are done (or the ones that aren't done
         * have failed too many times), we can stop. */
        if (egl_done && vk_done) break;
        if (!g_enable_gl) egl_done = 1;
        if (!g_enable_vk) vk_done = 1;
        if (egl_done && vk_done) break;

        usleep(200000);  /* 200ms */
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

    DBG("PID=%d sock=%s vk=%d gl=%d", getpid(), g_socket_path, enable_vk, enable_gl);

    /* The GLX path (glx_hook.c) sends frames TO an external idk-render process
     * via this client socket. The EGL path (egl_hook.c) creates its OWN
     * listening socket in compositor.c — this g_ipc_fd is NOT used there.
     *
     * On startup the idk-render process doesn't exist yet → connect() fails
     * with ENOENT. This is EXPECTED and not an error for the EGL path.
     * Only log it as info, not as "IPC connect failed" (which sounds alarming). */
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

    /* Start background thread to install hooks when GL/Vulkan libraries
     * are loaded by the game. This avoids the constructor crash (elfhacks +
     * code patching before linker is ready) while ensuring hooks ARE
     * installed even for games that dlopen libEGL at runtime (SDL, GLFW). */
    DBG("starting background hook installer thread");
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
    if (r != 0) {
        DBG("wayland input hook not installed (likely not a wayland client)");
    } else {
        DBG("wayland input hook installed");
    }
    return r;
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
