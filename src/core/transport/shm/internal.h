#ifndef IDK_TP_SHM_INTERNAL_H
#define IDK_TP_SHM_INTERNAL_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "core/shm_layout.h"
#include "core/transport.h"

/* _rsv[56]: [0..7]=shm_ptr, [8..39]=shm_name, [40..43]=eventfd, [44..47]=last_req_seq,
 *           [48..51]=cached_master_fd, [52..55]=cached_wv_fd */
#define TP_SH_SHM_PTR(rsv) (*(void **)(rsv))
#define TP_SH_SHM_NAME(rsv) ((char *)((rsv) + 8))
#define TP_SH_SHM_NAME_SIZE 32
#define TP_SH_EVENTFD(rsv) (*(int *)((rsv) + 40))
#define TP_SH_LAST_REQ_SEQ(rsv) (*(int *)((rsv) + 44))
#define TP_SH_CACHED_FD(rsv) (*(int *)((rsv) + 48))
#define TP_SH_CACHED_WV_FD(rsv) (*(int *)((rsv) + 52))

static inline void *shm_ptr(void *base, int offset) { return (char *)base + offset; }

static inline atomic_int *shm_atom(void *base, int offset) { return (atomic_int *)((char *)base + offset); }

static inline int32_t *shm_i32(void *base, int offset) { return (int32_t *)((char *)base + offset); }

/* Internal helpers: not part of the public symbol surface. */
#define IDK_INTERNAL __attribute__((visibility("hidden")))

IDK_INTERNAL int futex_wait(atomic_int *uaddr, int val, int timeout_ms);
IDK_INTERNAL int futex_wake(atomic_int *uaddr);

IDK_INTERNAL void *shm_setup(const char *name, int *out_fd, int is_creator);
IDK_INTERNAL void make_shm_name(const char *name, char *buf, size_t max);

IDK_INTERNAL int shm_init_consumer(idk_transport_t *tp, const char *name);
IDK_INTERNAL int shm_init_producer(idk_transport_t *tp, const char *name);

void tp_shm_destroy(idk_transport_t *tp);
void tp_shm_disconnect_client(idk_transport_t *tp);

IDK_INTERNAL void shm_start_health_thread(idk_transport_t *tp);
IDK_INTERNAL void shm_stop_health_thread(idk_transport_t *tp);

/* Steal producer-side fd `i` via pidfd_getfd (fd 0 cached + re-dup'd).
 * Returns the new fd, or -1. On any failure, closes fds[0..i-1]. */
IDK_INTERNAL int shm_steal_fd(idk_transport_t *tp, void *ptr, int target_fd, int i, int *fds);

#endif
