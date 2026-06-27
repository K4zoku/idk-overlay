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
#include <regex.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include "hook/overlay.h"
#include "hook/hook_plugin.h"
#include "hook/wayland_input.h"
#include "hook/x11_input.h"
#include "public/idk_ipc.h"
#include "core/log.h"
#include "core/compositor_common.h"

typedef unsigned long KeySym;

/* Capture hotkey globals — defined in wayland_input.c, shared with X11 */
extern uint32_t g_hotkey_keysym;
extern uint32_t g_hotkey_scancode;
extern uint32_t g_hotkey_mods;

static char g_socket_path[PATH_MAX];
static int g_enable_vk = 0;
static int g_enable_gl = 0;
static int g_initialized = 0;
static int g_wl_input_tried = 0;
static int g_x11_input_tried = 0;
static int g_x11_input_ok = 0;
static int g_hooks_installed = 0;
static int g_egl_hook_installed = 0;
static pid_t g_webview_pid = -1;

/* Overlay visibility — controlled by hotkey, checked by compositor render.
 * _Atomic (not volatile) so cross-thread reads/writes are well-defined
 * under C11. Written from input hooks (x11_kb / wayland_kb / sidecar),
 * read from compositor render path (egl_hook / glx_hook / vulkan_layer
 * / compositor_egl / compositor_vk). */
_Atomic int g_overlay_visible = 1;

/* Overlay hotkey config — separate from capture hotkey.
 * If both hotkeys are the same key, combined behavior:
 *   press when !captured → capture ON + show overlay
 *   press when captured  → capture OFF (overlay stays) */
uint32_t g_hotkey_overlay_keysym = 0;
uint32_t g_hotkey_overlay_scancode = 0;
uint32_t g_hotkey_overlay_mods = 0;

static idk_hook_plugin_t *g_plugins[] = {
    &idk_plugin_egl,
    &idk_plugin_glx,
};

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

static void *hook_install_thread(void *arg) {
    (void)arg;

    usleep(50000);

    /* Background retry for input hooks that may load late (Qt/SDL
     * platform plugins dlopen libX11/libwayland-client after our
     * constructor runs). Install BOTH — see comment in
     * idk_overlay_init() for why both are needed (XWayland vs
     * Wayland-native can't be reliably detected).
     *
     * The g_x11_input_tried / g_wl_input_tried latches prevent
     * double-install: each *_try_install_* function checks its own
     * latch and returns immediately if already tried. */
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

    int n_plugins = sizeof(g_plugins) / sizeof(g_plugins[0]);
    int done[n_plugins];
    memset(done, 0, sizeof(done));

    for (int i = 0; i < 150; i++) {
        for (int p = 0; p < n_plugins; p++) {
            if (done[p]) continue;

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

            /* Skip GLX if EGL already installed (app uses EGL, not GLX).
             * Prevents wasteful double-hooking on systems where both
             * libEGL and libGL are loaded. */
            if (strcmp(plug->name, "glx") == 0 && g_egl_hook_installed) {
                IDK_LOG("overlay", "glx: skipped (egl already hooked)\n");
                done[p] = 1;
                continue;
            }
            if (strcmp(plug->name, "egl") == 0 && g_hooks_installed &&
                !g_egl_hook_installed) {
                done[p] = 1;
                continue;
            }

            if (plugin_lib_loaded(plug)) {
                IDK_LOG("overlay", "%s: library found, installing hook\n", plug->name);
                int r = plug->init();
                if (r == 0) {
                    done[p] = 1;
                    g_hooks_installed = 1;
                    if (strcmp(plug->name, "egl") == 0) g_egl_hook_installed = 1;
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

static void get_process_name(char *buf, size_t bufsz) {
    buf[0] = '\0';
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, bufsz - 1);
        if (n > 0) { buf[n] = '\0'; char *nl = strchr(buf, '\n'); if (nl) *nl = '\0'; }
        close(fd);
    }
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

    char comm[64] = {0};
    get_process_name(comm, sizeof(comm));

    g_webview_pid = fork();
    if (g_webview_pid < 0) {
        IDK_ERR("overlay", "fork() failed: %s\n", strerror(errno));
        return;
    }

    if (g_webview_pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        for (int i = 3; i < 1024; i++) close(i);

        IDK_LOG("overlay", "forked webview child, exec %s (comm=%s socket=%s backend=%s)\n",
                bin, comm, g_socket_path, getenv("IDK_TP_BACKEND") ?: "socket");

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
        IDK_ERR("overlay", "execv(%s) failed: %s\n", bin, strerror(errno));
        _exit(127);
    }

    IDK_LOG("overlay", "webview forked (pid=%d, bin=%s)\n", (int)g_webview_pid, bin);
}

static void get_config_path(char *buf, size_t bufsz) {
    const char *env = getenv("IDK_CONFIG");
    if (env && env[0]) { snprintf(buf, bufsz, "%s", env); return; }
    const char *home = getenv("HOME"); if (!home) home = "/tmp";
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(buf, bufsz, "%s/idk-overlay.conf", xdg);
    else
        snprintf(buf, bufsz, "%s/.config/idk-overlay.conf", home);
}

static void parse_hotkey_str(const char *str, uint32_t *keysym, uint32_t *mods) {
    *keysym = 0; *mods = 0;
    if (!str || !str[0]) return;
    const char *keyname = str;
    const char *plus = strchr(str, '+');
    if (plus && plus > str) {
        size_t n = (size_t)(plus - str);
        char mod[32];
        if (n < sizeof(mod)) {
            memcpy(mod, str, n); mod[n] = '\0';
            if (strcasecmp(mod, "Shift") == 0) *mods = IDK_MOD_SHIFT;
            else if (strcasecmp(mod, "Ctrl") == 0) *mods = IDK_MOD_CTRL;
            else if (strcasecmp(mod, "Alt") == 0) *mods = IDK_MOD_ALT;
            else if (strcasecmp(mod, "Super") == 0) *mods = IDK_MOD_SUPER;
        }
        keyname = plus + 1;
    }
    static KeySym (*p_xstr)(const char *) = NULL;
    static int tried = 0;
    if (!tried) { tried = 1; void *lib = dlopen("libX11.so.6", RTLD_NOW|RTLD_NOLOAD); if (!lib) lib = dlopen("libX11.so.6", RTLD_NOW); if (lib) p_xstr = (KeySym(*)(const char*))dlsym(lib, "XStringToKeysym"); }
    if (p_xstr) { KeySym ks = p_xstr(keyname); if (ks) *keysym = (uint32_t)ks; }
    if (!*keysym) { if (!strcasecmp(keyname,"Tab")) *keysym=0xff09; else if (!strcasecmp(keyname,"F8")) *keysym=0xffc5; else if (!strcasecmp(keyname,"F9")) *keysym=0xffc6; else if (!strcasecmp(keyname,"F10")) *keysym=0xffc7; else if (!strcasecmp(keyname,"F11")) *keysym=0xffc8; else if (!strcasecmp(keyname,"F12")) *keysym=0xffc9; }
}

static void load_hotkey_config(void) {
    const char *env_cap = getenv("IDK_HOTKEY_CAPTURE"); if (!env_cap||!env_cap[0]) env_cap = "Shift+Tab";
    const char *env_ovl = getenv("IDK_HOTKEY_OVERLAY"); if (!env_ovl||!env_ovl[0]) env_ovl = "F8";
    parse_hotkey_str(env_cap, &g_hotkey_keysym, &g_hotkey_mods);
    parse_hotkey_str(env_ovl, &g_hotkey_overlay_keysym, &g_hotkey_overlay_mods);
    char proc[64]; get_process_name(proc, sizeof(proc));
    char cpath[PATH_MAX]; get_config_path(cpath, sizeof(cpath));
    FILE *f = fopen(cpath, "r");
    if (!f) { IDK_LOG("overlay", "hotkey: using defaults (cap=%s ovl=%s)\n", env_cap, env_ovl); return; }
    char line[512], cur_section[128]={0}; int in_match=0; char found_cap[128]={0}, found_ovl[128]={0};
    while (fgets(line, sizeof(line), f)) {
        char *s=line; while(*s==' '||*s=='\t')s++; char *e=s+strlen(s)-1; while(e>s&&(*e=='\n'||*e=='\r'||*e==' '))*e--='\0';
        if (*s=='#'||*s==';'||!*s) continue;
        if (*s=='[') { char *c=strchr(s,']'); if(c){*c='\0'; snprintf(cur_section,sizeof(cur_section),"%s",s+1); in_match=0;} continue; }
        char *eq=strchr(s,'='); if(!eq)continue; *eq='\0'; char *v=eq+1; while(*v==' ')v++;
        if (!strcasecmp(s,"Match")) { if(proc[0]&&v[0]){regex_t re; if(regcomp(&re,v,REG_NOSUB|REG_EXTENDED)==0){if(regexec(&re,proc,0,NULL,0)==0)in_match=1; regfree(&re);}} }
        else if(in_match){ if(!strcasecmp(s,"HotkeyCapture"))snprintf(found_cap,sizeof(found_cap),"%s",v); else if(!strcasecmp(s,"HotkeyOverlay"))snprintf(found_ovl,sizeof(found_ovl),"%s",v); }
    }
    fclose(f);
    if(found_cap[0]){parse_hotkey_str(found_cap,&g_hotkey_keysym,&g_hotkey_mods); IDK_LOG("overlay","hotkey: capture='%s' from config matching '%s'\n",found_cap,proc);}
    if(found_ovl[0]){parse_hotkey_str(found_ovl,&g_hotkey_overlay_keysym,&g_hotkey_overlay_mods); IDK_LOG("overlay","hotkey: overlay='%s' from config matching '%s'\n",found_ovl,proc);}
}

int idk_overlay_init(const char *socket_path, int enable_vk, int enable_gl) {
    if (g_initialized) return 0;
    g_initialized = 1;
    g_enable_vk = enable_vk;
    g_enable_gl = enable_gl;

    if (socket_path)
        snprintf(g_socket_path, sizeof(g_socket_path), "%s", socket_path);
    else
        idk_comp_get_default_socket_path(g_socket_path, sizeof(g_socket_path), 0);

    load_hotkey_config();

    fork_webview();

    /* Install BOTH X11 and Wayland input hooks if their libs are loaded.
     * Use RTLD_NOLOAD so we only detect libs the game has already loaded
     * (dlopen(RTLD_NOW) would force-load libX11 even for Wayland-native
     * games, making the X11 probe always succeed and masking Wayland).
     *
     * Why both? On XWayland, the game loads libX11 (for X events) AND
     * libwayland-client (Qt/SDL platform plugins) — but only X11 events
     * fire (game uses XNextEvent, not wl_keyboard). On Wayland-native,
     * only Wayland events fire (wl_keyboard, not XNextEvent).
     *
     * Both hooks share the same globals (g_captured, g_hotkey_pressed,
     * g_overlay_visible) and the same input socket (init_input_socket
     * guards against double-init). Hotkey detection is idempotent —
     * g_hotkey_pressed latch prevents double-toggle if both paths fire
     * (shouldn't happen, but defense in depth).
     *
     * Without both hooks:
     *   - Wayland-native with only X11 hooks → no hotkey (XNextEvent
     *     never called by game)
     *   - XWayland with only Wayland hooks → no hotkey (wl_keyboard
     *     listener never receives the game's key events; they go to
     *     XNextEvent instead)
     *   - Detecting which one to use is unreliable (libwayland can be
     *     loaded as a plugin dependency even for XWayland games)
     *
     * So: install both, let only the active path fire. */
    void *xh = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (!xh) xh = dlopen("libX11.so", RTLD_NOW | RTLD_NOLOAD);
    if (xh) {
        dlclose(xh);  /* RTLD_NOLOAD just checks, doesn't add refcount */
        idk_overlay_try_install_x11_input();
    }

    void *wlh = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!wlh) wlh = dlopen("libwayland-client.so", RTLD_NOW | RTLD_NOLOAD);
    if (wlh) {
        dlclose(wlh);
        idk_overlay_try_install_wayland_input();
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

    if (g_webview_pid > 0) {
        kill(g_webview_pid, SIGTERM);
        int status;
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
