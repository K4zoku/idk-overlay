#include <errno.h>
#include <time.h>

#include "internal.h"

int tp_shm_send_request(idk_transport_t *tp, const idk_request_msg_t *req) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready || !req) {
    errno = EINVAL;
    return -1;
  }
  atomic_int *req_seq = shm_atom(ptr, SHM_O_REQ_SEQ);
  atomic_fetch_add(req_seq, 1);
  atomic_thread_fence(memory_order_release);
  futex_wake(req_seq);
  return 0;
}

int tp_shm_recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready || !req) {
    errno = EINVAL;
    return -1;
  }

  int *last_seq = &TP_SH_LAST_REQ_SEQ(tp->_rsv);
  atomic_int *req_seq = shm_atom(ptr, SHM_O_REQ_SEQ);

  struct timespec deadline;
  if (timeout_ms >= 0) {
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)timeout_ms * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
  }

  while (1) {
    int cur = atomic_load(req_seq);
    if (cur != *last_seq) {
      *last_seq = cur;
      req->type = IDK_REQUEST_NEXT_FRAME;
      return 0;
    }

    if (timeout_ms >= 0) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
        errno = ETIMEDOUT;
        return -1;
      }
    }

    futex_wait(req_seq, cur, 100);
  }
}
