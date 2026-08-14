#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "core/compositor.h"
#include "core/log.h"

/* Broker decision state — shared with overlay.c via extern.
 *   0=pending, 1=done-broker, 2=done-no-broker, 3=failed */
_Atomic int g_broker_state = 2;
pthread_mutex_t g_broker_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_broker_cond = PTHREAD_COND_INITIALIZER;

/* ===== idk_compositor_t singleton ===== */

idk_compositor_t g_comp = {0};

/* ===== Shared compositor API ===== */

int idk_compositor_init(void) {
  if (g_comp.inited)
    return 0;

  pthread_mutex_lock(&g_broker_lock);
  while (atomic_load(&g_broker_state) == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    int rc = pthread_cond_timedwait(&g_broker_cond, &g_broker_lock, &ts);
    if (rc == ETIMEDOUT)
      break;
  }
  pthread_mutex_unlock(&g_broker_lock);

  char path[512];
  idk_comp_get_path(path, sizeof(path));
  if (idk_tp_init(&g_comp.tp, IDK_TP_CONSUMER, path) != 0)
    return -1;
  idk_tp_accept(&g_comp.tp);
  g_comp.inited = true;
  return 0;
}

int idk_compositor_has_frame(void) { return g_comp.has_frame ? 1 : 0; }

void idk_compositor_shutdown(void) {
  if (g_comp.inited) {
    idk_tp_destroy(&g_comp.tp);
    g_comp.inited = false;
  }
}
