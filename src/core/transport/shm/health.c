#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "internal.h"

static void tp_shm_health_check(void *ptr) {
  int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
  if (prod_pid > 0 && kill(prod_pid, 0) < 0 && errno == ESRCH)
    atomic_store(shm_atom(ptr, SHM_O_PROD_STATE), -1);
  int cons_pid = *shm_i32(ptr, SHM_O_CONS_PID);
  if (cons_pid > 0 && kill(cons_pid, 0) < 0 && errno == ESRCH)
    atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), -1);
}

#define MAX_SHM_TP 4

static struct {
  idk_transport_t *tp;
  pthread_t thread;
  atomic_bool stop;
} g_shm_health[MAX_SHM_TP];

static void *shm_health_loop(void *arg) {
  idk_transport_t *tp = arg;
  while (1) {
    bool stop;
    for (int i = 0; i < MAX_SHM_TP; i++) {
      if (g_shm_health[i].tp != tp)
        continue;
      stop = atomic_load(&g_shm_health[i].stop);
      break;
    }
    if (stop)
      break;
    for (int i = 0; i < 20 && !stop; i++) {
      usleep(100000);
      for (int j = 0; j < MAX_SHM_TP; j++) {
        if (g_shm_health[j].tp != tp)
          continue;
        stop = atomic_load(&g_shm_health[j].stop);
        break;
      }
    }
    if (stop)
      break;
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (ptr)
      tp_shm_health_check(ptr);
  }
  return NULL;
}

void shm_start_health_thread(idk_transport_t *tp) {
  for (int i = 0; i < MAX_SHM_TP; i++) {
    if (g_shm_health[i].tp)
      continue;
    g_shm_health[i].tp = tp;
    atomic_store(&g_shm_health[i].stop, false);
    pthread_create(&g_shm_health[i].thread, NULL, shm_health_loop, tp);
    return;
  }
}

void shm_stop_health_thread(idk_transport_t *tp) {
  for (int i = 0; i < MAX_SHM_TP; i++) {
    if (g_shm_health[i].tp != tp)
      continue;
    atomic_store(&g_shm_health[i].stop, true);
    pthread_join(g_shm_health[i].thread, NULL);
    g_shm_health[i].tp = NULL;
    return;
  }
}
