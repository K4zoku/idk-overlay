/* Layer-side runtime: shared atomics, lazy broker bootstrap, vkCreateInstance hook.
 *
 * CRITICAL: this library has NO __attribute__((constructor)) on_load().
 * Everything below starts lazily from idk_layer_ensure_started(), which
 * the vkCreateInstance hook calls (i.e. only when a real game creates
 * a Vulkan instance). */
#ifdef IDK_HAVE_VK_LAYER

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#include "core/broker.h"
#include "core/compositor_vk.h"
#include "core/log.h"
#include "hook/overlay.h"
#include "internal.h"

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

/* Stored during CreateInstance for compositor_vk to load instance-level functions */
static VkInstance g_vk_instance = VK_NULL_HANDLE;
static PFN_vkGetInstanceProcAddr g_vk_instance_gpa = NULL;

VkInstance idk_vk_layer_get_instance(void) { return g_vk_instance; }
PFN_vkGetInstanceProcAddr idk_vk_layer_get_instance_gpa(void) { return g_vk_instance_gpa; }

/* Hook: vkCreateInstance */
IDK_INTERNAL VKAPI_ATTR VkResult VKAPI_CALL idk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                               const VkAllocationCallbacks *pAllocator,
                                                               VkInstance *pInstance) {
  VkLayerInstanceCreateInfo *chain_info = idk_vk_layer_get_instance_chain(pCreateInfo);
  if (!chain_info) {
    IDK_ERR("vk-layer", "CreateInstance: no layer link info in chain\n");
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
  if (!fpCreateInstance) {
    IDK_ERR("vk-layer", "CreateInstance: next layer has no vkCreateInstance\n");
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  idk_vk_layer_chain_advance_instance(chain_info);

  VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
  if (result != VK_SUCCESS)
    return result;

  pthread_mutex_lock(&g_dispatch_lock);
  struct instance_dispatch *id = new_instance(*pInstance);
  pthread_mutex_unlock(&g_dispatch_lock);

  if (id) {
    id->GetInstanceProcAddr = fpGetInstanceProcAddr;
    id->DestroyInstance = (PFN_vkDestroyInstance)fpGetInstanceProcAddr(*pInstance, "vkDestroyInstance");
  }

  IDK_LOG("vk-layer", "CreateInstance OK (instance=%p)\n", (void *)*pInstance);

  g_vk_instance = *pInstance;
  g_vk_instance_gpa = fpGetInstanceProcAddr;

  idk_layer_ensure_started();
  idk_vk_compositor_notify_resize(0, 0);
  idk_overlay_try_install_wayland_input();

  return result;
}

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

#endif /* IDK_HAVE_VK_LAYER */
