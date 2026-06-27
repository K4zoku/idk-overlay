#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "hook/overlay.h"
#include "hook/hook_plugin.h"
#include "hook/wayland_input.h"
#include "hook/x11_input.h"
#include "core/log.h"

static char g_socket_path[PATH_MAX];
static int g_enable_vk = 0;
static int g_enable_gl = 0;
static int g_initialized = 0;
static int g_wl_input_tried = 0;
static int g_x11_input_tried = 0;
static int g_x11_input_ok = 0;
static int g_hooks_installed = 0;
static pid_t g_webview_pid = -1;

/* All registered hook plugins */
static idk_hook_plugin_t *g_plugins[] = {
    &idk_plugin_egl,
    &idk_plugin_glx,
    &idk_plugin_vk_syringe,
};

/* Probe a plugin by trying to dlopen its libraries (NOLOAD) */
static int plugin_lib_loaded(const idk_hook_plugin_t *p) {
    for (int i = 0; p->lib_patterns[i]; i++) {
        void *h = dlopen(p->lib_patterns[i], RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            dlclose(h);
            return 1;
        }
    }
    return 0;
}

/* Background thread: poll for graphics libraries and install hooks */
static void *hook_install_thread(void *arg) {
    (void)arg;

    usleep(50000);

    /* Probe for X11 FIRST. XWayland games load both libX11 and libwayland,
     * but should use X11 input (not Wayland) for event interception.
     * If X11 input hook installs successfully, skip Wayland input to avoid
     * double-toggle (both hooks share g_captured/g_hotkey_pressed). */
    for (int i = 0; i < 150 && !g_x11_input_tried; i++) {
        void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libX11.so", RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            dlclose(h);
            idk_overlay_try_install_x11_input();
            break;
        }
        usleep(10000);
    }

    /* Only install Wayland input if X11 input failed.
     * This prevents double-toggle when both libs are loaded (XWayland). */
    if (!g_x11_input_ok) {
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
    }

    int n_plugins = sizeof(g_plugins) / sizeof(g_plugins[0]);
    int done[n_plugins];
    memset(done, 0, sizeof(done));

    for (int i = 0; i < 150; i++) {
        for (int p = 0; p < n_plugins; p++) {
            if (done[p]) continue;

            /* Skip based on global enable flags */
            idk_hook_plugin_t *plug = g_plugins[p];
            int enabled = 0;
            if (strcmp(plug->name, "vk-syringe") == 0)
                enabled = g_enable_vk;
            else
                enabled = g_enable_gl;

            if (!enabled) {
                done[p] = 1;
                continue;
            }

            if (plugin_lib_loaded(plug)) {
                IDK_LOG("overlay", "%s: library found, installing hook\n", plug->name);
                int r = plug->init();
                if (r == 0) {
                    done[p] = 1;
                    g_hooks_installed = 1;
                    IDK_LOG("overlay", "%s: hook installed OK\n", plug->name);
                } else {
                    IDK_LOG("overlay", "%s: hook install failed, will retry\n", plug->name);
                }
            }
        }

        int all_done = 1;
        for (int p = 0; p < n_plugins; p++)
            if (!done[p]) { all_done = 0; break; }
        if (all_done) break;

        usleep(200000);
    }

    if (!g_hooks_installed)
        IDK_LOG("overlay", "no graphics hooks installed after 30s\n");

    return NULL;
}

/* ── Webview fork+exec ───────────────────────────────────────────────── */

/* Locate the webview binary. Priority:
 *   1. IDK_WEBVIEW_BIN env var (explicit path)
 *   2. PATH search for "idk-webview"
 * Returns 0 and fills buf on success, -1 on failure. */
static int find_webview_bin(char *buf, size_t bufsz) {
    const char *env = getenv("IDK_WEBVIEW_BIN");
    if (env && env[0]) {
        snprintf(buf, bufsz, "%s", env);
        return 0;
    }

    /* Search PATH for "idk-webview" */
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < PATH_MAX - 32) {
            snprintf(buf, bufsz, "%.*s/idk-webview", (int)len, p);
            if (access(buf, X_OK) == 0) return 0;
        }
        if (!colon) break;
        p = colon + 1;
    }
    return -1;
}

/* Fork+exec the webview process as a child of the game.
 * The child inherits IDK_SOCKET, IDK_TP_BACKEND (default "shm"), and
 * any IDK_* env vars. Game name is passed via --match so the webview
 * can select the right config section. */
static void fork_webview(void) {
    char bin[PATH_MAX];
    if (find_webview_bin(bin, sizeof(bin)) != 0) {
        IDK_LOG("overlay", "webview binary not found (set IDK_WEBVIEW_BIN or install idk-webview in PATH)\n");
        return;
    }

    /* Default to SHM backend for forked webview (faster, no socket overhead).
     * User can override by setting IDK_TP_BACKEND before launching the game. */
    setenv("IDK_TP_BACKEND", "shm", 0);  /* don't overwrite if already set */

    /* Pass game process name so webview can match config sections.
     * Read from /proc/self/comm (truncated to 15 chars by kernel). */
    char comm[64] = {0};
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        if (n > 0) {
            comm[n] = '\0';
            /* Strip trailing newline */
            char *nl = strchr(comm, '\n');
            if (nl) *nl = '\0';
        }
        close(fd);
    }

    g_webview_pid = fork();
    if (g_webview_pid < 0) {
        IDK_ERR("overlay", "fork() failed: %s\n", strerror(errno));
        return;
    }

    if (g_webview_pid == 0) {
        /* Child — exec webview */
        IDK_LOG("overlay", "forked webview child, exec %s (comm=%s socket=%s backend=%s)\n",
                bin, comm, g_socket_path, getenv("IDK_TP_BACKEND") ?: "socket");

        /* Build argv: idk-webview --socket <path> --match <comm> */
        char *argv[8];
        int ai = 0;
        argv[ai++] = bin;
        argv[ai++] = "--socket";
        argv[ai++] = g_socket_path;
        if (comm[0]) {
            argv[ai++] = "--match";
            argv[ai++] = comm;
        }
        argv[ai] = NULL;

        execv(bin, argv);
        /* If execv returns, it failed */
        IDK_ERR("overlay", "execv(%s) failed: %s\n", bin, strerror(errno));
        _exit(127);
    }

    /* Parent — webview is now running as child */
    IDK_LOG("overlay", "webview forked (pid=%d, bin=%s)\n", (int)g_webview_pid, bin);
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

    /* Fork+exec webview as child process. Done before any threads spawn
     * (fork in multi-threaded process is unsafe — only async-signal-safe
     * functions can be called between fork and exec). The constructor
     * runs single-threaded, so this is safe. */
    fork_webview();

    /* Try X11 input first (synchronous probe). XWayland games should use X11. */
    void *xh = dlopen("libX11.so.6", RTLD_NOW);
    if (!xh) xh = dlopen("libX11.so", RTLD_NOW);
    if (xh) {
        idk_overlay_try_install_x11_input();
        dlclose(xh);
    }

    /* Only try Wayland if X11 failed. */
    if (!g_x11_input_ok) {
        void *wlh = dlopen("libwayland-client.so.0", RTLD_NOW);
        if (!wlh) wlh = dlopen("libwayland-client.so", RTLD_NOW);
        if (wlh) {
            idk_overlay_try_install_wayland_input();
            dlclose(wlh);
        }
    }

    pthread_t t;
    if (pthread_create(&t, NULL, hook_install_thread, NULL) == 0)
        pthread_detach(t);

    return 0;
}

void idk_overlay_shutdown(void) {
    int n_plugins = sizeof(g_plugins) / sizeof(g_plugins[0]);
    for (int p = 0; p < n_plugins; p++)
        g_plugins[p]->shutdown();

    idk_wayland_input_shutdown();
    idk_x11_input_shutdown();

    /* Kill forked webview child if still running */
    if (g_webview_pid > 0) {
        kill(g_webview_pid, SIGTERM);
        int status;
        /* Brief wait, then SIGKILL if still alive */
        if (waitpid(g_webview_pid, &status, WNOHANG) == 0) {
            usleep(100000);  /* 100ms */
            kill(g_webview_pid, SIGKILL);
            waitpid(g_webview_pid, &status, 0);
        }
        g_webview_pid = -1;
    }
}

int idk_overlay_try_install_wayland_input(void) {
    if (g_wl_input_tried) return 0;
    g_wl_input_tried = 1;
    int r = idk_wayland_input_init();
    return r;
}

int idk_overlay_try_install_x11_input(void) {
    if (g_x11_input_tried) return g_x11_input_ok ? 0 : -1;
    g_x11_input_tried = 1;
    int r = idk_x11_input_init();
    g_x11_input_ok = (r == 0);
    return r;
}

__attribute__((constructor))
static void on_load(void) {
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
