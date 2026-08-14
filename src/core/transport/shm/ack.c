#include <string.h>

#include "internal.h"

void tp_shm_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready || !ack)
    return;

  memcpy(shm_ptr(ptr, SHM_O_ACK), ack, sizeof(*ack));
  atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_ACK);
  futex_wake(shm_atom(ptr, SHM_O_SLOT_STATE));
}
