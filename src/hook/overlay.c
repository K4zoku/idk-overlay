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
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <stdarg.h>
#include <poll.h>
#include <time.h>

#include "hook/overlay.h"
#include "hook/hook_plugin.h"
#include "hook/wayland_input.h"
#include "hook/x11_input.h"
#include "public/idk_ipc.h"
#include "core/log.h"
#include "core/compositor.h"

typedef unsigned long KeySym;

/* Capture hotkey globals - defined in wayland_input.c, shared with X11 */
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
int g_input_eventfd = -1;
static int g_use_broker = 0;
static int g_broker_fd = -1;

/* Broker state defined in compositor.c (needed by test static lib) */
extern _Atomic int      g_broker_state;
extern pthread_mutex_t  g_broker_lock;
extern pthread_cond_t   g_broker_cond;

extern _Atomic int g_captured;

_Atomic int g_webview_dead = 0;
static int g_webview_crash_count = 0;
static time_t g_webview_last_fork_time = 0;

/* Overlay visibility - controlled by hotkey, checked by compositor render.
 * _Atomic (not volatile) so cross-thread reads/writes are well-defined
 * under C11. Written from input hooks (x11_kb / wayland_kb / sidecar),
 * read from compositor render path (egl_hook / glx_hook / vulkan_layer
 * / compositor_egl / compositor_vk). */
_Atomic int g_overlay_visible = 1;

/* Overlay hotkey config - separate from capture hotkey.
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

static void fork_webview(void);
static void webview_disable(void);
static int  connect_via_broker(void);
static bool detect_wine(void);

/* Cached result of detect_wine() — evaluated once during overlay init.
 * /proc/self/maps scanning is the costliest check, do it sparingly. */
static int g_wine_detected = -1;

/* Abstract-namespace socket name of this overlay's broker control
 * channel. Builder kept here so webview exec env (set by broker) and
 * the overlay-side connect share the exact same string. */
static void make_broker_name(char *buf, size_t bufsz) {
    buf[0] = '\0';
    snprintf(buf + 1, bufsz - 1, "idk_broker_%d", (int)getuid());
}

static socklen_t abstract_addrlen(const char *name) {
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name + 1));
}

static bool detect_wine(void) {
    if (g_wine_detected >= 0) return g_wine_detected == 1;

    const char *name = idk_process_name();
    size_t len = name ? strlen(name) : 0;
    if (len > 4 && strcasecmp(name + len - 4, ".exe") == 0) {
        g_wine_detected = 1;
        return true;
    }

    /* /proc/self/maps: a single pass looking for Wine's loader or its
     * runtime libs. /usr/lib/wine is the distro install path; libwine/
     * ntdll cover the in-game loaded modules. Cache the result. */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { g_wine_detected = 0; return false; }
    char line[1024];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        if (strstr(line, "/libwine") || strstr(line, "/ntdll") ||
            strstr(line, "/usr/lib/wine") || strstr(line, "/usr/lib64/wine")) {
            found = 1;
        }
    }
    fclose(f);
    g_wine_detected = found ? 1 : 0;
    return found == 1;
}

/* Phase 2 broker path (Mode 2): connect to the host-namespace broker,
 * send handshake describing the transport/input sockets the webview
 * should connect to, then wait for an ack. Returns 0 on success and
 * sets g_use_broker so the rest of the overlay skips fork_webview and
 * the compositor uses abstract sockets via IDK_TP_ABSTRACT env. */
static int connect_via_broker(void) {
    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cfd < 0) {
        IDK_ERR("overlay", "broker: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    char bname[64];
    make_broker_name(bname, sizeof(bname));
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t blen = 1 + strlen(bname + 1);
    memcpy(addr.sun_path, bname, blen);
    if (connect(cfd, (struct sockaddr *)&addr, abstract_addrlen(bname)) < 0) {
        IDK_LOG("overlay", "broker: connect \\0%s failed: %s\n",
                bname + 1, strerror(errno));
        close(cfd);
        return -1;
    }

    char tp_plain[64];   /* no leading NUL — stored in handshake */
    char in_plain[64];
    snprintf(tp_plain, sizeof(tp_plain), "idk_tp_%d", (int)getpid());
    snprintf(in_plain, sizeof(in_plain), "idk_input_%d", (int)getpid());
    /* Mirror into the local env so the compositor/wayland input pick
     * the matching abstract names up when they call idk_comp_get_path /
     * init_input_socket. */
    setenv("IDK_TP_ABSTRACT",     tp_plain, 1);
    setenv("IDK_INPUT_ABSTRACT",  in_plain, 1);

    idk_cp_handshake_t hs;
    memset(&hs, 0, sizeof(hs));
    hs.identity = IDK_CP_ID_OVERLAY;
    hs.tp_backend = 0; /* socket backend forced in broker mode */
    const char *comm = idk_process_name();
    if (comm) snprintf(hs.comm, sizeof(hs.comm), "%s", comm);
    snprintf(hs.tp_socket,    sizeof(hs.tp_socket),    "%s", tp_plain);
    snprintf(hs.input_socket, sizeof(hs.input_socket), "%s", in_plain);

    size_t total = 0;
    while (total < sizeof(hs)) {
        ssize_t n = write(cfd, (const char *)&hs + total, sizeof(hs) - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            IDK_ERR("overlay", "broker: handshake write failed: %s\n", strerror(errno));
            close(cfd);
            return -1;
        }
        total += (size_t)n;
    }

    struct pollfd pfd = { .fd = cfd, .events = POLLIN };
    if (poll(&pfd, 1, 5000) <= 0) {
        IDK_ERR("overlay", "broker: ack timeout\n");
        close(cfd);
        return -1;
    }
    uint8_t ack = 0xff;
    ssize_t r = read(cfd, &ack, 1);
    if (r != 1 || ack != 0x00) {
        IDK_ERR("overlay", "broker: ack failed (r=%zd ack=0x%02x)\n", r, ack);
        close(cfd);
        return -1;
    }

    /* Keep cfd open — its EOF is the signal that the broker (and its
     * webview child) is gone. Future monitor thread could read it. */
    fcntl(cfd, F_SETFD, FD_CLOEXEC);
    g_broker_fd = cfd; /* leak intentionally */
    IDK_LOG("overlay", "broker: connected (comm=%s tp=%s input=%s)\n",
            hs.comm, hs.tp_socket, hs.input_socket);
    return 0;
}

static void *broker_connect_thread(void *arg) {
    (void)arg;
    detect_wine();
    bool want_broker = (g_wine_detected == 1);
    const char *broker_env = getenv("IDK_BROKER");
    bool broker_forced = broker_env && broker_env[0];
    if (broker_forced) want_broker = true;

    if (want_broker && connect_via_broker() == 0) {
        g_use_broker = 1;
        pthread_mutex_lock(&g_broker_lock);
        atomic_store(&g_broker_state, 1);
        pthread_cond_signal(&g_broker_cond);
        pthread_mutex_unlock(&g_broker_lock);
        IDK_LOG("overlay", "broker mode active — fork_webview skipped\n");
    } else if (want_broker && broker_forced) {
        pthread_mutex_lock(&g_broker_lock);
        atomic_store(&g_broker_state, 3);
        pthread_cond_signal(&g_broker_cond);
        pthread_mutex_unlock(&g_broker_lock);
        IDK_ERR("overlay", "IDK_BROKER forced but broker unreachable — overlay disabled\n");
        webview_disable();
        close(g_broker_fd); g_broker_fd = -1;
    } else {
        if (want_broker)
            IDK_LOG("overlay", "wine detected, broker unavailable — fallback fork_webview\n");
        pthread_mutex_lock(&g_broker_lock);
        atomic_store(&g_broker_state, 2);
        pthread_cond_signal(&g_broker_cond);
        pthread_mutex_unlock(&g_broker_lock);
    }
    return NULL;
}

static void *hook_install_thread(void *arg) {
    (void)arg;

    usleep(50000);

    /* ── Phase 1: Wait for a graphics library to load ───────────────*/
    int gl_detected = 0;
    for (int i = 0; i < 1200; i++) {
        if (g_enable_gl) {
            void *h;
            h = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
            if (!h) h = dlopen("libGL.so", RTLD_NOW | RTLD_NOLOAD);
            if (h) { dlclose(h); gl_detected = 1; break; }
            h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
            if (!h) h = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
            if (h) { dlclose(h); gl_detected = 1; break; }
        }
        if (g_enable_vk) {
            void *h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
            if (!h) h = dlopen("libvulkan.so", RTLD_NOW | RTLD_NOLOAD);
            if (h) { dlclose(h); gl_detected = 1; break; }
        }
        usleep(50000);  /* 50ms */
    }

    if (!gl_detected) {
        IDK_LOG("overlay", "no GL/Vulkan library loaded after 60s — not a game process, exiting\n");
        return NULL;
    }

    IDK_LOG("overlay", "graphics library detected — initializing overlay\n");

    /* ── Phase 2: wait for broker decision (from broker_connect_thread) ─ */
    for (int i = 0; i < 100 && atomic_load(&g_broker_state) == 0; i++)
        usleep(50000);

    int state = atomic_load(&g_broker_state);
    if (state == 1) {
        IDK_LOG("overlay", "broker mode active — fork_webview skipped\n");
    } else if (state == 3) {
        IDK_ERR("overlay", "IDK_BROKER forced but broker unreachable — overlay disabled\n");
        return NULL;
    } else {
        fork_webview();
    }

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

    /* ── Phase 3: Install graphics hooks ─────────────────────────── */
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

static void webview_disable(void) {
    g_webview_dead = 1;
    g_overlay_visible = 0;
    g_captured = 0;
}

/* Locate the webview binary. Priority:
 *   1. IDK_WEBVIEW_BIN env var (explicit path)
 *   2. PATH search for "idk-webview"
 * Returns 0 and fills buf on success, -1 on failure. */
static int find_webview_bin(char *buf, size_t bufsz) {
    const char *env = getenv("IDK_WEBVIEW_BIN");
    if (env && env[0]) { snprintf(buf, bufsz, "%s", env); return 0; }
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

/* Monitor thread — waits for the webview process to exit via waitpid.
 * Distinguishes user-close from crash using exit code + heuristic. */
static void *webview_monitor(void *arg) {
    pid_t pid = (pid_t)(intptr_t)arg;
    int status;
    if (waitpid(pid, &status, 0) <= 0)
        return NULL;

    if (g_webview_dead)
        return NULL;

    g_webview_pid = -1;

    IDK_LOG("overlay", "webview exit: pid=%d status=0x%x\n", (int)pid, status);
    if (WIFEXITED(status))
        IDK_LOG("overlay", "  exit code=%d\n", WEXITSTATUS(status));
    if (WIFSIGNALED(status))
        IDK_LOG("overlay", "  signal=%d\n", WTERMSIG(status));

    int user_closed = 0;
    if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) == 127) {
            /* exec failed — treat as crash, will retry */
            IDK_LOG("overlay", "webview exec failed (exit=127)\n");
        } else if (WEXITSTATUS(status) == 0) {
            user_closed = 1;
        } else {
            time_t elapsed = time(NULL) - g_webview_last_fork_time;
            if (elapsed >= 2)
                user_closed = 1;
            else
                IDK_LOG("overlay", "webview exited too fast (exit=%d, %lds)\n",
                        WEXITSTATUS(status), (long)elapsed);
        }
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGTERM || sig == SIGKILL)
            user_closed = 1;
        else
            IDK_LOG("overlay", "webview killed by signal %d\n", sig);
    }

    if (user_closed) {
        IDK_LOG("overlay", "webview (pid=%d) closed by user - overlay disabled\n", (int)pid);
        webview_disable();
        return NULL;
    }

    time_t now = time(NULL);
    if (g_webview_last_fork_time > 0 && now - g_webview_last_fork_time > 30)
        g_webview_crash_count = 0;

    g_webview_crash_count++;
    g_webview_last_fork_time = now;

    if (g_webview_crash_count > 3) {
        IDK_ERR("overlay", "webview crashed %d times - giving up, disabling overlay\n",
                g_webview_crash_count);
        webview_disable();
        return NULL;
    }

    IDK_LOG("overlay", "webview crashed (count=%d) - reforking\n", g_webview_crash_count);
    fork_webview();
    return NULL;
}

/* Fork+exec the webview process. Uses syscall(SYS_execve, ...) to
 * bypass glibc wrappers. If exec fails in a Wine-isolated environment,
 * the child exits with code 127 and the monitor will refork. */
static void fork_webview(void) {
    char bin[PATH_MAX];
    if (find_webview_bin(bin, sizeof(bin)) != 0) {
        IDK_LOG("overlay", "webview binary not found (set IDK_WEBVIEW_BIN or install idk-webview in PATH)\n");
        return;
    }

    if (g_input_eventfd < 0) {
        g_input_eventfd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
        if (g_input_eventfd >= 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", g_input_eventfd);
            setenv("IDK_INPUT_EVENTFD", buf, 1);
        }
    }

    g_webview_last_fork_time = time(NULL);
    g_webview_pid = fork();
    if (g_webview_pid < 0) {
        IDK_ERR("overlay", "fork() failed: %s\n", strerror(errno));
        return;
    }

    if (g_webview_pid == 0) {
        const char *comm = idk_process_name();

        for (int i = 3; i < 1024; i++) {
            if (i == g_input_eventfd) continue;
            close(i);
        }

        extern char **environ;
        for (int ei = 0; environ[ei]; ei++) {
            if (strncmp(environ[ei], "LD_PRELOAD=", 11) == 0) {
                char *val = environ[ei] + 11;
                if (!*val) break;
                char *newval = val, *w = val;
                while (*newval) {
                    while (*newval == ':' || *newval == ' ') newval++;
                    if (!*newval) break;
                    char *start = newval;
                    while (*newval && *newval != ':') newval++;
                    if ((size_t)(newval - start) > 0 && !strstr(start, "libidk-overlay.so")) {
                        if (w > val) *w++ = ':';
                        size_t slen = (size_t)(newval - start);
                        memmove(w, start, slen);
                        w += slen;
                    }
                }
                *w = '\0';
                if (w == val) {
                    for (int ej = ei; environ[ej]; ej++)
                        environ[ej] = environ[ej + 1];
                }
                break;
            }
        }

        char *argv[] = {bin, "--socket", g_socket_path, "--match", (char *)comm, NULL};
        IDK_LOG("overlay", "forked webview child, exec %s (comm=%s socket=%s)\n",
                bin, comm[0] ? comm : "", g_socket_path);
        syscall(SYS_execve, bin, argv, environ);
        _exit(127);
    }

    IDK_LOG("overlay", "webview forked (pid=%d, bin=%s)\n", (int)g_webview_pid, bin);

    pthread_t t;
    if (pthread_create(&t, NULL, webview_monitor, (void *)(intptr_t)g_webview_pid) == 0)
        pthread_detach(t);
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
    const char *proc = idk_process_name();
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

/* Intercept prctl(PR_SET_NAME) to refresh the cached process ident
 * when a game renames itself (used by Wine/Proton, Steam child procs). */
int prctl(int option, ...) {
    va_list ap;
    va_start(ap, option);
    unsigned long a2 = va_arg(ap, unsigned long);
    unsigned long a3 = va_arg(ap, unsigned long);
    unsigned long a4 = va_arg(ap, unsigned long);
    unsigned long a5 = va_arg(ap, unsigned long);
    va_end(ap);

    int ret = syscall(SYS_prctl, option, a2, a3, a4, a5);
    if (option == PR_SET_NAME && ret == 0)
        idk_process_ident_invalidate();
    return ret;
}

/* Skip overlay init in known non-game child processes (awk, shell
 * interpreters, updaters, etc.) that inherit LD_PRELOAD from the
 * parent. These short-lived tools crash or misbehave when the overlay
 * constructor creates threads and calls dlopen. The overlay must keep
 * LD_PRELOAD set so the real game binary (often exec'd after AppImage
 * extraction / Flatpak launch) still receives the injection. */
static int idk_is_target_process(void) {
    /* Check /proc/self/exe for AppImage-internal binaries. The FUSE
     * mount path always starts with /tmp/.mount_<name>.<random>/. */
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        if (strstr(exe, "/.mount_"))
            return 1;
    }

    /* Fallback: check comm against known system-tool names that are
     * never the target game. */
    char comm[64] = {0};
    int _fd = open("/proc/self/comm", O_RDONLY);
    if (_fd < 0) return 1; /* can't check, allow */
    ssize_t _n = read(_fd, comm, sizeof(comm) - 1);
    close(_fd);
    if (_n <= 0) return 1;
    comm[_n] = '\0';
    if (_n > 0 && comm[_n - 1] == '\n') comm[_n - 1] = '\0';

    static const char *blacklist[] = {
        "awk", "gawk", "mawk",
        "sh", "bash", "dash",
        "UpdateNix",
        "mktemp", "cp", "mv", "rm", "chmod",
        "readlink", "dirname", "basename",
        "head", "tail", "cut", "grep", "sed",
        "true", "false",
        "explorer.exe",
        "services.exe",
        "wineboot.exe",
        "wineserver",
        "msiexec.exe",
        "rundll32.exe",
        "cmd.exe",
        "reg.exe",
        "schtasks.exe",
        "wineboot",
        "wine",
        "wine64-preloader",
        "wine-preloader",
        NULL
    };
    for (int i = 0; blacklist[i]; i++) {
        if (strcmp(comm, blacklist[i]) == 0)
            return 0;
    }
    return 1;
}

int idk_overlay_init(const char *socket_path, int enable_vk, int enable_gl) {
    if (g_initialized) return 0;

    g_initialized = 1;
    atomic_store(&g_broker_state, 2);
    g_enable_vk = enable_vk;
    g_enable_gl = enable_gl;

    if (socket_path)
        snprintf(g_socket_path, sizeof(g_socket_path), "%s", socket_path);
    else
        idk_comp_get_default_socket_path(g_socket_path, sizeof(g_socket_path), 0);

    load_hotkey_config();

    /* External caller (syringe/inject): no broker, hook_install_thread forks */
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
            usleep(100000);
            kill(g_webview_pid, SIGKILL);
            waitpid(g_webview_pid, &status, 0);
        }
        g_webview_pid = -1;
    }

    if (g_input_eventfd >= 0) { close(g_input_eventfd); g_input_eventfd = -1; }
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
    if (!idk_is_target_process())
        return;

    g_initialized = 1;
    atomic_store(&g_broker_state, 0);

    const char *env_vk = getenv("IDK_VK");
    const char *env_gl = getenv("IDK_GL");
    const char *env_path = getenv("IDK_SOCKET");
    g_enable_vk = env_vk ? atoi(env_vk) : 0;
    g_enable_gl = env_gl ? atoi(env_gl) : 1;
    if (env_path)
        snprintf(g_socket_path, sizeof(g_socket_path), "%s", env_path);
    else
        idk_comp_get_default_socket_path(g_socket_path, sizeof(g_socket_path), 0);

    load_hotkey_config();

    pthread_t broker_t;
    if (pthread_create(&broker_t, NULL, broker_connect_thread, NULL) == 0)
        pthread_detach(broker_t);

    pthread_t t;
    if (pthread_create(&t, NULL, hook_install_thread, NULL) == 0)
        pthread_detach(t);
}

__attribute__((destructor))
static void on_unload(void) {
    idk_overlay_shutdown();
}
