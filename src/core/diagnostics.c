#include "core/diagnostics.h"

#include <stdatomic.h>

static _Atomic uint64_t g_counters[IDK_DIAG_COUNTER_COUNT];

void idk_diag_inc(enum idk_diag_counter counter) {
  if ((unsigned)counter < IDK_DIAG_COUNTER_COUNT)
    atomic_fetch_add_explicit(&g_counters[counter], 1, memory_order_relaxed);
}

uint64_t idk_diag_get(enum idk_diag_counter counter) {
  if ((unsigned)counter >= IDK_DIAG_COUNTER_COUNT)
    return 0;
  return atomic_load_explicit(&g_counters[counter], memory_order_relaxed);
}

void idk_diag_reset(void) {
  for (unsigned i = 0; i < IDK_DIAG_COUNTER_COUNT; i++)
    atomic_store_explicit(&g_counters[i], 0, memory_order_relaxed);
}
