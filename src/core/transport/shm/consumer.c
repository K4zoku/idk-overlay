#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/log.h"

#include "internal.h"

static void close_frame_fds(int fds[4], int *nfd) {
  for (int i = 0; i < 4; i++) {
    if (fds[i] >= 0) {
      close(fds[i]);
      fds[i] = -1;
    }
  }
  *nfd = 0;
}

int shm_init_consumer(idk_transport_t *tp, const char *name) {
  char shm_name[64];
  make_shm_name(name, shm_name, sizeof(shm_name));
  size_t namelen = strlen(shm_name);
  if (namelen >= TP_SH_SHM_NAME_SIZE)
    namelen = TP_SH_SHM_NAME_SIZE - 1;
  memcpy(TP_SH_SHM_NAME(tp->_rsv), shm_name, namelen + 1);

  shm_unlink(shm_name);

  int shm_fd;
  void *ptr = shm_setup(shm_name, &shm_fd, 1);
  if (!ptr)
    return -1;

  *shm_i32(ptr, SHM_O_MAGIC) = SHM_MAGIC_VAL;
  atomic_store(shm_atom(ptr, SHM_O_PROD_STATE), 0);
  atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 1);
  *shm_i32(ptr, SHM_O_CONS_PID) = (int32_t)getpid();
  *shm_i32(ptr, SHM_O_PROD_PID) = 0;
  atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);
  atomic_store(shm_atom(ptr, SHM_O_FRAME_SEQ), 0);
  atomic_store(shm_atom(ptr, SHM_O_CURSOR_SEQ), 0);

  const char *efd_env = getenv("IDK_INPUT_EVENTFD");
  int efd = efd_env ? atoi(efd_env) : 0;
  if (efd < 0)
    efd = 0;
  *shm_i32(ptr, SHM_O_EVENTFD) = efd;
  TP_SH_EVENTFD(tp->_rsv) = efd;

  tp->_server_fd = shm_fd;
  tp->_client_fd = -1;
  TP_SH_SHM_PTR(tp->_rsv) = ptr;
  tp->ready = false;

  shm_start_health_thread(tp);

  IDK_LOG("tp", "shm: consumer created %s (pid=%d, waiting for producer)\n", shm_name, (int)getpid());
  return 0;
}

int tp_shm_accept(idk_transport_t *tp) {
  if (tp->ready)
    return 1;

  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr) {
    if (tp->_server_fd < 0 && tp->_client_fd < 0)
      return -1;
    return -1;
  }

  int ps = atomic_load(shm_atom(ptr, SHM_O_PROD_STATE));
  if (ps != 1) {
    if (ps == -1)
      return -1;
    return 0;
  }

  int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
  if (prod_pid <= 0)
    return -1;

  atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 2);
  futex_wake(shm_atom(ptr, SHM_O_CONS_STATE));
  tp->ready = true;
  IDK_LOG("tp", "shm: consumer ready, producer pid=%d (pidfd deferred)\n", prod_pid);
  return 1;
}

int tp_shm_poll(idk_transport_t *tp) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready)
    return -1;

  if (atomic_load(shm_atom(ptr, SHM_O_PROD_STATE)) == -1)
    return -1;

  int s = atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE));
  return (s == SLOT_FRAME) ? 1 : 0;
}

int tp_shm_recv(idk_transport_t *tp, idk_frame_header_t *hdr, int fds[4], int *nfd) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready || !hdr || !fds || !nfd) {
    errno = EINVAL;
    return -1;
  }

  for (int i = 0; i < 4; i++)
    fds[i] = -1;
  *nfd = 0;

  if (atomic_load(shm_atom(ptr, SHM_O_PROD_STATE)) == -1)
    return -1;

  int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
  if (prod_pid > 0 && kill(prod_pid, 0) < 0 && errno == ESRCH) {
    IDK_ERR("tp", "shm: producer pid=%d dead (crashed without graceful shutdown)\n", prod_pid);
    return -1;
  }

  if (atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE)) != SLOT_FRAME)
    return 0;

  memcpy(hdr, shm_ptr(ptr, SHM_O_HDR), sizeof(*hdr));
  int count = *shm_i32(ptr, SHM_O_DMABUF_NFD);
  if (count < 1 || count > 4) {
    atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);
    futex_wake(shm_atom(ptr, SHM_O_SLOT_STATE));
    errno = EPROTO;
    return -1;
  }

  for (int i = 0; i < count; i++) {
    int target_fd = shm_i32(ptr, SHM_O_DMABUF_FDS)[i];
    fds[i] = shm_steal_fd(tp, ptr, target_fd, i, fds);
    if (fds[i] < 0) {
      close_frame_fds(fds, nfd);
      return -1;
    }
    *nfd = i + 1;
  }

  atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_CONSUMED);
  return 1;
}

int tp_shm_drop_frame(idk_transport_t *tp) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (!ptr || !tp->ready)
    return -1;

  if (atomic_load(shm_atom(ptr, SHM_O_PROD_STATE)) == -1)
    return -1;

  if (atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE)) != SLOT_FRAME)
    return 0;

  atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);
  futex_wake(shm_atom(ptr, SHM_O_SLOT_STATE));
  return 1;
}
