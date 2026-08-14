/* Layer-side runtime for libidk-vklayer.so.
 *
 * This translation unit is the Vulkan-layer counterpart of overlay.c:
 * it owns the shared state (g_overlay_visible, g_webview_dead) and the
 * broker bootstrap that the preload path gets from its constructor.
 *
 * CRITICAL: this library has NO __attribute__((constructor)) on_load().
 * The Vulkan loader dlopens libidk-vklayer.so into *any* Vulkan app
 * (vulkaninfo, vkcube, ...). A constructor here would run full overlay
 * init (threads, webview fork) in those processes — exactly what
 * crashed the session when the preload lib was reused as a layer.
 * Everything below starts lazily from idk_layer_ensure_started(), which
 * vulkan_layer.c calls inside its vkCreateInstance hook (i.e. only when
 * a real game creates a Vulkan instance). */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#include "core/broker.h"
#include "core/log.h"

/* Overlay visibility + webview liveness — same symbols the GL/EGL path
 * defines in overlay.c, needed by compositor_vk.c. Each library (preload
 * vs layer) carries its own copy; only one is loaded per process. */
_Atomic int g_overlay_visible = 1;
_Atomic int g_webview_dead = 0;

/* Input hooks (wayland_input.c / x11_input.c) live in the preload lib.
 * Layer mode v1: no input capture — overlay render only. */
int idk_overlay_try_install_wayland_input(void) { return 0; }
int idk_overlay_try_install_x11_input(void) { return 0; }

/* Broker state defined in compositor.c */
extern _Atomic int g_broker_state;
extern pthread_mutex_t g_broker_lock;
extern pthread_cond_t g_broker_cond;

static int g_layer_started = 0;

int idk_layer_ensure_started(void) {
  if (g_layer_started)
    return 0;
  g_layer_started = 1;

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
      IDK_LOG("overlay", "broker mode active (layer) — webview spawned by broker\n");
    } else if (broker_forced) {
      atomic_store(&g_broker_state, 3);
      pthread_cond_signal(&g_broker_cond);
      IDK_ERR("overlay", "IDK_BROKER forced but broker unreachable — overlay disabled\n");
      g_webview_dead = 1;
    } else {
      IDK_LOG("overlay", "wine detected, broker unavailable — falling back\n");
      atomic_store(&g_broker_state, 2);
      pthread_cond_signal(&g_broker_cond);
    }
  } else {
    atomic_store(&g_broker_state, 2);
    pthread_cond_signal(&g_broker_cond);
  }
  return 0;
}
