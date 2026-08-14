#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/compositor.h"
#include "hook/overlay.h"
#include "hook/wayland_input.h"
#include "hook/x11_input.h"

#include "internal.h"

/* Cross-thread overlay state - read by the render backends in this lib. */
_Atomic int g_webview_dead = 0;
_Atomic int g_overlay_visible = 1;

int g_input_eventfd = -1;

/* Overlay hotkey config - separate from capture hotkey.
 * If both hotkeys are the same key, combined behavior:
 *   press when !captured -> capture ON + show overlay
 *   press when captured  -> capture OFF (overlay stays) */
uint32_t g_hotkey_overlay_keysym = 0;
uint32_t g_hotkey_overlay_scancode = 0;
uint32_t g_hotkey_overlay_mods = 0;

IDK_INTERNAL char g_socket_path[PATH_MAX];
IDK_INTERNAL int g_enable_vk = 0;
IDK_INTERNAL int g_enable_gl = 0;
IDK_INTERNAL pid_t g_webview_pid = -1;

static int g_initialized = 0;

int idk_overlay_init(const char *socket_path, int enable_vk, int enable_gl) {
  if (g_initialized)
    return 0;
  g_initialized = 1;
  atomic_store(&g_broker_state, 2);
  g_enable_vk = enable_vk;
  g_enable_gl = enable_gl;
  if (socket_path)
    snprintf(g_socket_path, sizeof(g_socket_path), "%s", socket_path);
  else
    idk_comp_get_default_socket_path(g_socket_path, sizeof(g_socket_path), 0);
  load_hotkey_config();
  pthread_t t;
  if (pthread_create(&t, NULL, hook_install_thread, NULL) == 0)
    pthread_detach(t);
  return 0;
}

void idk_overlay_shutdown(void) {
  int n_plugins = plugins_count();
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
  if (g_input_eventfd >= 0) {
    close(g_input_eventfd);
    g_input_eventfd = -1;
  }
}

__attribute__((constructor)) static void on_load(void) {
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
  pthread_t t;
  if (pthread_create(&t, NULL, hook_install_thread, NULL) == 0)
    pthread_detach(t);
}

__attribute__((destructor)) static void on_unload(void) { idk_overlay_shutdown(); }
