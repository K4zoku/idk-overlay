#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

int futex_wait(atomic_int *uaddr, int val, int timeout_ms) {
  struct timespec ts, *tsp = NULL;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    tsp = &ts;
  }
  int op = FUTEX_WAIT;
  return (int)syscall(__NR_futex, uaddr, op, val, tsp, NULL, 0);
}

int futex_wake(atomic_int *uaddr) { return (int)syscall(__NR_futex, uaddr, FUTEX_WAKE, 1, NULL, NULL, 0); }
