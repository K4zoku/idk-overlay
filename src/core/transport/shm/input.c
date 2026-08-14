#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

int tp_shm_send_input(idk_transport_t *tp, const idk_input_event_t *ev) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready || !ev) {
    errno = EINVAL;
    return -1;
  }

  if (atomic_load(shm_atom(ptr, SHM_O_CONS_STATE)) == -1) {
    errno = ECONNRESET;
    return -1;
  }

  int is_critical = (ev->type == IDK_INPUT_STATE || ev->type == IDK_INPUT_OVERLAY);

  if (atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE)) != SLOT_EMPTY) {
    if (!is_critical) {
      errno = EAGAIN;
      return -1;
    }
    int spun = 0;
    while (atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE)) != SLOT_EMPTY) {
      if (++spun > 50) {
        errno = EAGAIN;
        return -1;
      }
      usleep(100);
    }
    if (atomic_load(shm_atom(ptr, SHM_O_CONS_STATE)) == -1) {
      errno = ECONNRESET;
      return -1;
    }
  }

  memcpy(shm_ptr(ptr, SHM_O_HDR), ev, sizeof(*ev));
  atomic_fetch_add(shm_atom(ptr, SHM_O_FRAME_SEQ), 1);
  atomic_thread_fence(memory_order_release);
  atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_FRAME);

  int efd = TP_SH_EVENTFD(tp->_rsv);
  if (efd > 0) {
    uint64_t val = 1;
    write(efd, &val, 8);
  }

  return 0;
}

int tp_shm_recv_input(idk_transport_t *tp, idk_input_event_t *ev) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !ev) {
    errno = EINVAL;
    return -1;
  }
  if (atomic_load(shm_atom(ptr, SHM_O_PROD_STATE)) == -1) {
    return -1;
  }

  if (atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE)) != SLOT_FRAME) {
    return 0;
  }

  atomic_thread_fence(memory_order_acquire);
  memcpy(ev, shm_ptr(ptr, SHM_O_HDR), sizeof(*ev));
  atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);

  return 1;
}
