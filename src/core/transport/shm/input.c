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

static int cursor_valid(const idk_cursor_update_t *cursor, size_t capacity) {
  if (!cursor || cursor->magic != IDK_CURSOR_MAGIC || cursor->version != IDK_CURSOR_VERSION || cursor->visible > 1 ||
      cursor->data_size > IDK_CURSOR_MAX_BYTES || cursor->data_size > capacity)
    return 0;
  if (!cursor->visible)
    return cursor->data_size == 0;
  if (cursor->shape == IDK_CURSOR_CUSTOM)
    return cursor->width > 0 && cursor->width <= IDK_CURSOR_MAX_DIM && cursor->height > 0 &&
           cursor->height <= IDK_CURSOR_MAX_DIM && cursor->data_size == (uint32_t)cursor->width * cursor->height * 4u;
  return cursor->shape >= IDK_CURSOR_DEFAULT && cursor->shape <= IDK_CURSOR_ALL_RESIZE && cursor->width == 0 &&
         cursor->height == 0 && cursor->data_size == 0;
}

int tp_shm_send_cursor(idk_transport_t *tp, const idk_cursor_update_t *cursor, const uint8_t *pixels) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready || !cursor || (cursor->data_size > 0 && !pixels) ||
      !cursor_valid(cursor, SHM_CURSOR_CAPACITY)) {
    errno = EINVAL;
    return -1;
  }
  atomic_int *seqp = shm_atom(ptr, SHM_O_CURSOR_SEQ);
  int seq = atomic_load_explicit(seqp, memory_order_relaxed);
  if (seq & 1)
    seq++;
  atomic_store_explicit(seqp, seq + 1, memory_order_relaxed);
  memcpy(shm_ptr(ptr, SHM_O_CURSOR_HDR), cursor, sizeof(*cursor));
  if (cursor->data_size > 0)
    memcpy(shm_ptr(ptr, SHM_O_CURSOR_DATA), pixels, cursor->data_size);
  atomic_thread_fence(memory_order_release);
  atomic_store_explicit(seqp, seq + 2, memory_order_release);
  return 0;
}

int tp_shm_recv_cursor(idk_transport_t *tp, idk_cursor_update_t *cursor, uint8_t *pixels, size_t capacity) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !cursor) {
    errno = EINVAL;
    return -1;
  }
  atomic_int *seqp = shm_atom(ptr, SHM_O_CURSOR_SEQ);
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t seq = (uint32_t)atomic_load_explicit(seqp, memory_order_acquire);
    if (seq == 0 || seq == tp->_cursor_seq || (seq & 1))
      return 0;
    memcpy(cursor, shm_ptr(ptr, SHM_O_CURSOR_HDR), sizeof(*cursor));
    if (!cursor_valid(cursor, capacity) || (cursor->data_size > 0 && !pixels)) {
      errno = EBADMSG;
      return -1;
    }
    if (cursor->data_size > 0)
      memcpy(pixels, shm_ptr(ptr, SHM_O_CURSOR_DATA), cursor->data_size);
    atomic_thread_fence(memory_order_acquire);
    uint32_t stable = (uint32_t)atomic_load_explicit(seqp, memory_order_acquire);
    if (stable == seq) {
      tp->_cursor_seq = stable;
      return 1;
    }
  }
  return 0;
}
