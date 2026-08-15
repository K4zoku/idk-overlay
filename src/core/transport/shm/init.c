#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.h"

#include "internal.h"

void make_shm_name(const char *name, char *buf, size_t max) {
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  snprintf(buf, max, "/%s", base);
}

void *shm_setup(const char *name, int *out_fd, int is_creator) {
  int flags = is_creator ? (O_CREAT | O_RDWR) : O_RDWR;
  mode_t mode = is_creator ? 0600 : 0;

  int fd = shm_open(name, flags, mode);
  if (fd < 0)
    return NULL;

  if (is_creator && ftruncate(fd, SHM_SIZE) < 0) {
    close(fd);
    return NULL;
  }

  void *ptr = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  *out_fd = fd;
  return ptr;
}

int tp_shm_init(idk_transport_t *tp, const char *name) {
  if (tp->role == IDK_TP_CONSUMER)
    return shm_init_consumer(tp, name);
  else
    return shm_init_producer(tp, name);
}

void tp_shm_destroy(idk_transport_t *tp) {
  shm_stop_health_thread(tp);

  char shm_name_save[40] = {0};
  const char *sn = TP_SH_SHM_NAME(tp->_rsv);
  if (sn[0])
    memcpy(shm_name_save, sn, sizeof(shm_name_save) - 1);

  void *ptr = TP_SH_SHM_PTR(tp->_rsv);
  if (ptr) {
    if (tp->role == IDK_TP_CONSUMER) {
      atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), -1);
      futex_wake(shm_atom(ptr, SHM_O_CONS_STATE));
    }
    munmap(ptr, SHM_SIZE);
  }
  if (tp->_server_fd >= 0) {
    close(tp->_server_fd);
    tp->_server_fd = -1;
  }
  if (tp->_client_fd >= 0) {
    close(tp->_client_fd);
    tp->_client_fd = -1;
  }
  if (TP_SH_CACHED_FD(tp->_rsv) >= 0) {
    close(TP_SH_CACHED_FD(tp->_rsv));
    TP_SH_CACHED_FD(tp->_rsv) = -1;
  }
  tp->ready = false;

  memset(tp->_rsv, 0, sizeof(tp->_rsv));

  if (shm_name_save[0] && tp->role == IDK_TP_CONSUMER)
    shm_unlink(shm_name_save);
}

void tp_shm_disconnect_client(idk_transport_t *tp) {
  void *ptr = TP_SH_SHM_PTR(tp->_rsv);

  if (tp->role == IDK_TP_CONSUMER && ptr) {
    atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);
    *shm_i32(ptr, SHM_O_PROD_PID) = 0;
    *shm_i32(ptr, SHM_O_DMABUF_NFD) = 0;
    atomic_store(shm_atom(ptr, SHM_O_FRAME_SEQ), 0);
    atomic_store(shm_atom(ptr, SHM_O_REQ_SEQ), 0);
    atomic_store(shm_atom(ptr, SHM_O_CURSOR_SEQ), 0);
    tp->_cursor_seq = 0;

    if (tp->_client_fd >= 0) {
      close(tp->_client_fd);
      tp->_client_fd = -1;
    }
    if (TP_SH_CACHED_FD(tp->_rsv) >= 0) {
      close(TP_SH_CACHED_FD(tp->_rsv));
      TP_SH_CACHED_FD(tp->_rsv) = -1;
    }
    TP_SH_CACHED_WV_FD(tp->_rsv) = 0;

    atomic_store(shm_atom(ptr, SHM_O_PROD_STATE), 0);
    atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 1);
    futex_wake(shm_atom(ptr, SHM_O_CONS_STATE));

    IDK_LOG("tp", "shm: consumer soft-disconnected (pidfd closed, SHM kept open for reconnect)\n");
  } else if (tp->role == IDK_TP_PRODUCER) {
    tp_shm_destroy(tp);
    return;
  }

  tp->ready = false;
}
