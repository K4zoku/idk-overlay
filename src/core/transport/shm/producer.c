#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"

#include "internal.h"

int shm_init_producer(idk_transport_t *tp, const char *name) {
  char shm_name[64];
  make_shm_name(name, shm_name, sizeof(shm_name));
  size_t namelen = strlen(shm_name);
  if (namelen >= TP_SH_SHM_NAME_SIZE)
    namelen = TP_SH_SHM_NAME_SIZE - 1;
  memcpy(TP_SH_SHM_NAME(tp->_rsv), shm_name, namelen + 1);

  int shm_fd;
  void *ptr = NULL;

  for (int i = 0; i < 300; i++) {
    ptr = shm_setup(shm_name, &shm_fd, 0);
    if (ptr)
      break;
    if (errno == ENOENT || errno == EACCES) {
      usleep(100000);
      continue;
    }
    return -1;
  }
  if (!ptr) {
    IDK_ERR("tp", "shm: producer timeout waiting for SHM %s\n", shm_name);
    return -1;
  }

  if (*shm_i32(ptr, SHM_O_MAGIC) != SHM_MAGIC_VAL) {
    IDK_ERR("tp", "shm: bad magic on %s\n", shm_name);
    close(shm_fd);
    return -1;
  }

  int efd = *shm_i32(ptr, SHM_O_EVENTFD);
  if (efd < 0)
    efd = 0;
  TP_SH_EVENTFD(tp->_rsv) = efd;

  *shm_i32(ptr, SHM_O_PROD_PID) = (int32_t)getpid();
  atomic_store(shm_atom(ptr, SHM_O_PROD_STATE), 1);
  futex_wake(shm_atom(ptr, SHM_O_PROD_STATE));

  tp->_server_fd = -1;
  tp->_client_fd = shm_fd;
  TP_SH_SHM_PTR(tp->_rsv) = ptr;

  IDK_LOG("tp", "shm: producer waiting for consumer on %s\n", shm_name);
  atomic_int *cons_state = shm_atom(ptr, SHM_O_CONS_STATE);
  int waited_ms = 0;
  while (atomic_load(cons_state) != 2) {
    if (atomic_load(cons_state) == -1) {
      IDK_ERR("tp", "shm: consumer died before ready on %s\n", shm_name);
      close(shm_fd);
      tp->_client_fd = -1;
      return -1;
    }
    int cur_state = atomic_load(cons_state);
    int rc = futex_wait(cons_state, cur_state, 1000);
    (void)rc;
    waited_ms += 1000;
    if (waited_ms >= 30000) {
      IDK_ERR("tp", "shm: timeout waiting for consumer on %s\n", shm_name);
      close(shm_fd);
      tp->_client_fd = -1;
      return -1;
    }
  }

  tp->ready = true;
  shm_start_health_thread(tp);
  int cons_pid = *shm_i32(ptr, SHM_O_CONS_PID);
  IDK_LOG("tp", "shm: producer ready on %s (consumer pid=%d)\n", shm_name, cons_pid);
  return 0;
}

int tp_shm_send(idk_transport_t *tp, const idk_frame_header_t *hdr, const int *fds, int nfd) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready || !hdr || !fds || nfd < 1 || nfd > 4) {
    errno = EINVAL;
    return -1;
  }

  int cons_pid = *shm_i32(ptr, SHM_O_CONS_PID);
  if (cons_pid > 0 && kill(cons_pid, 0) < 0 && errno == ESRCH) {
    IDK_ERR("tp", "shm: consumer pid=%d dead\n", cons_pid);
    errno = ECONNRESET;
    return -1;
  }

  if (atomic_load(shm_atom(ptr, SHM_O_CONS_STATE)) == -1) {
    errno = ECONNRESET;
    return -1;
  }

  for (int tries = 0;; tries++) {
    int s = atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE));
    if (s == SLOT_EMPTY || s == SLOT_ACK)
      break;
    if (s == -1 || atomic_load(shm_atom(ptr, SHM_O_CONS_STATE)) == -1) {
      errno = ECONNRESET;
      return -1;
    }
    if (tries >= 500) {
      errno = EAGAIN;
      return -1;
    }
    futex_wait(shm_atom(ptr, SHM_O_SLOT_STATE), s, 2);
  }

  *shm_i32(ptr, SHM_O_DMABUF_NFD) = nfd;
  for (int i = 0; i < nfd; i++)
    shm_i32(ptr, SHM_O_DMABUF_FDS)[i] = fds[i];

  memcpy(shm_ptr(ptr, SHM_O_HDR), hdr, sizeof(*hdr));

  atomic_fetch_add(shm_atom(ptr, SHM_O_FRAME_SEQ), 1);

  atomic_thread_fence(memory_order_release);

  atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_FRAME);
  futex_wake(shm_atom(ptr, SHM_O_SLOT_STATE));

  return 0;
}

int tp_shm_wait_ack(idk_transport_t *tp, idk_ack_msg_t *ack, int timeout_ms) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready || !ack) {
    errno = EINVAL;
    return -1;
  }

  struct timespec deadline;
  if (timeout_ms >= 0) {
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)timeout_ms * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
  }

  atomic_int *slot = shm_atom(ptr, SHM_O_SLOT_STATE);
  while (1) {
    int s = atomic_load(slot);
    if (s == SLOT_ACK)
      break;

    if (s == -1 || atomic_load(shm_atom(ptr, SHM_O_CONS_STATE)) == -1) {
      errno = ECONNRESET;
      return -1;
    }
    if (timeout_ms >= 0) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
        errno = ETIMEDOUT;
        return -1;
      }
    }

    int rc = futex_wait(slot, s, 100);
    (void)rc;
  }

  atomic_thread_fence(memory_order_acquire);
  memcpy(ack, shm_ptr(ptr, SHM_O_ACK), sizeof(*ack));
  atomic_store(slot, SLOT_EMPTY);
  futex_wake(slot);

  return 0;
}
