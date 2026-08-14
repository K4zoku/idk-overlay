#ifndef IDK_PRELOAD_INTERNAL_H
#define IDK_PRELOAD_INTERNAL_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "hook/hook_plugin.h"

/* Internal helpers: not part of the public symbol surface. */
#define IDK_INTERNAL __attribute__((visibility("hidden")))

/* Shared preload state. */
IDK_INTERNAL extern char g_socket_path[PATH_MAX];
IDK_INTERNAL extern int g_enable_vk;
IDK_INTERNAL extern int g_enable_gl;
IDK_INTERNAL extern int g_hooks_installed;
IDK_INTERNAL extern int g_egl_hook_installed;
IDK_INTERNAL extern pid_t g_webview_pid;
IDK_INTERNAL extern time_t g_webview_last_fork_time;
IDK_INTERNAL extern idk_hook_plugin_t *g_plugins[];

/* Exported state. */
extern _Atomic int g_webview_dead;
extern _Atomic int g_overlay_visible;
extern _Atomic int g_captured;
extern int g_input_eventfd;
extern uint32_t g_hotkey_overlay_keysym;
extern uint32_t g_hotkey_overlay_scancode;
extern uint32_t g_hotkey_overlay_mods;

/* Capture hotkey globals - defined in wayland_input.c, shared with X11 */
extern uint32_t g_hotkey_keysym;
extern uint32_t g_hotkey_scancode;
extern uint32_t g_hotkey_mods;

/* Broker state defined in core/compositor/state.c */
extern _Atomic int g_broker_state;
extern pthread_mutex_t g_broker_lock;
extern pthread_cond_t g_broker_cond;

/* Cross-file helpers. */
IDK_INTERNAL int idk_is_target_process(void);
IDK_INTERNAL int lib_loaded(const char *name);
IDK_INTERNAL int plugin_lib_loaded(const idk_hook_plugin_t *p);
IDK_INTERNAL void load_hotkey_config(void);
IDK_INTERNAL int find_webview_bin(char *buf, size_t bufsz);
IDK_INTERNAL void fork_webview(void);
IDK_INTERNAL void webview_disable(void);
IDK_INTERNAL void *webview_monitor(void *arg);
IDK_INTERNAL void *hook_install_thread(void *arg);
IDK_INTERNAL int plugins_count(void);
IDK_INTERNAL void plugins_install_try(int *done, int n_plugins);

#endif /* IDK_PRELOAD_INTERNAL_H */
