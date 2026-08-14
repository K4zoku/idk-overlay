#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "core/broker.h"
#include "core/log.h"
#include "hook/overlay.h"
#include "hook/wayland_input.h"
#include "hook/x11_input.h"

#include "internal.h"

static int g_wl_input_tried = 0;
static int g_x11_input_tried = 0;
static int g_x11_input_ok = 0;

/* Poll for the game's graphics library (60s max, 50ms steps). */
static int phase1_poll_gl(void) {
  int gl_detected = 0;
  for (int i = 0; i < 1200; i++) {
    if (g_enable_gl) {
      if (lib_loaded("libGL.so.1") || lib_loaded("libGL.so") || lib_loaded("libEGL.so.1") || lib_loaded("libEGL.so")) {
        gl_detected = 1;
        break;
      }
    }
    if (lib_loaded("libvulkan.so.1") || lib_loaded("libvulkan.so")) {
      gl_detected = 1;
      g_enable_vk = 1;
      break;
    }
    usleep(50000);
  }
  return gl_detected;
}

/* Broker decision + webview spawn. Returns 0 to abort the thread
 * (broker forced but unreachable). */
static int phase2_broker_decision(void) {
  detect_wine();
  bool want_broker = (g_wine_detected == 1);
  const char *broker_env = getenv("IDK_BROKER");
  bool broker_forced = broker_env && broker_env[0];
  if (broker_forced)
    want_broker = true;
  if (want_broker) {
    if (connect_via_broker() == 0) {
      g_use_broker = 1;
      atomic_store(&g_broker_state, 1);
      pthread_cond_signal(&g_broker_cond);
      IDK_LOG("overlay", "broker mode active — fork_webview skipped\n");
    } else if (broker_forced) {
      atomic_store(&g_broker_state, 3);
      pthread_cond_signal(&g_broker_cond);
      IDK_ERR("overlay", "IDK_BROKER forced but broker unreachable — overlay disabled\n");
      webview_disable();
      close(g_broker_fd);
      g_broker_fd = -1;
      return 0;
    } else {
      IDK_LOG("overlay", "wine detected, broker unavailable — fallback fork_webview\n");
      atomic_store(&g_broker_state, 2);
      pthread_cond_signal(&g_broker_cond);
    }
  } else {
    atomic_store(&g_broker_state, 2);
    pthread_cond_signal(&g_broker_cond);
  }
  int state = atomic_load(&g_broker_state);
  if (state == 1) {
  } else if (state == 3) {
    return 0;
  } else {
    fork_webview();
  }
  return 1;
}

/* Install graphics hooks once their libraries show up (30s max). */
static void phase3_install_plugins(void) {
  int n_plugins = plugins_count();
  int done[n_plugins];
  memset(done, 0, sizeof(done));
  if (g_hooks_installed)
    done[1] = 1;
  for (int i = 0; i < 150; i++) {
    plugins_install_try(done, n_plugins);
    int all_done = 1;
    for (int p = 0; p < n_plugins; p++)
      if (!done[p]) {
        all_done = 0;
        break;
      }
    if (all_done)
      break;
    usleep(200000);
  }
  if (!g_hooks_installed)
    IDK_LOG("overlay", "no graphics hooks installed after 30s\n");
}

static void probe_x11_input(void) {
  for (int i = 0; i < 150 && !g_x11_input_tried; i++) {
    void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (!h)
      h = dlopen("libX11.so", RTLD_NOW | RTLD_NOLOAD);
    if (h) {
      dlclose(h);
      idk_overlay_try_install_x11_input();
      break;
    }
    usleep(10000);
  }
}

static void probe_wayland_input(void) {
  for (int i = 0; i < 150 && !g_wl_input_tried; i++) {
    if (idk_is_wine()) {
      g_wl_input_tried = 1;
      break;
    }
    void *h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!h)
      h = dlopen("libwayland-client.so", RTLD_NOW | RTLD_NOLOAD);
    if (h) {
      dlclose(h);
      idk_overlay_try_install_wayland_input();
      break;
    }
    usleep(10000);
  }
}

static void probe_input_backends(void) {
  probe_x11_input();
  probe_wayland_input();
}

IDK_INTERNAL void *hook_install_thread(void *arg) {
  (void)arg;
  usleep(50000);
  if (!phase1_poll_gl()) {
    IDK_LOG("overlay", "no GL/Vulkan library loaded after 60s — not a game process, exiting\n");
    atomic_store(&g_broker_state, 2);
    pthread_cond_signal(&g_broker_cond);
    return NULL;
  }
  IDK_LOG("overlay", "graphics library detected (gl=%d vk=%d)\n", g_enable_gl, g_enable_vk);
  if (!phase2_broker_decision())
    return NULL;
  phase3_install_plugins();
  probe_input_backends();
  return NULL;
}

int idk_overlay_try_install_wayland_input(void) {
  if (g_wl_input_tried)
    return 0;
  g_wl_input_tried = 1;
  return idk_wayland_input_init();
}

int idk_overlay_try_install_x11_input(void) {
  if (g_x11_input_tried)
    return g_x11_input_ok ? 0 : -1;
  g_x11_input_tried = 1;
  int r = idk_x11_input_init();
  g_x11_input_ok = (r == 0);
  return r;
}
