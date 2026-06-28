#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/eventfd.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>

#include "core/transport.h"
#include "core/log.h"

#define SHM_SIZE 4096

#define SHM_O_MAGIC       0
#define SHM_O_PROD_STATE  4
#define SHM_O_CONS_STATE  8
#define SHM_O_PROD_PID    12
#define SHM_O_CONS_PID    16
#define SHM_O_DMABUF_NFD  20
#define SHM_O_DMABUF_FDS  24
#define SHM_O_HDR         40
#define SHM_O_ACK         68
#define SHM_O_SLOT_STATE  80
#define SHM_O_FRAME_SEQ   84
#define SHM_O_REQ_SEQ     88
#define SHM_O_EVENTFD     92

_Static_assert(SHM_O_EVENTFD + 4 <= SHM_SIZE,
               "SHM layout exceeds page size");

#define SHM_MAGIC_VAL 0x4D485349

#define SLOT_EMPTY   0
#define SLOT_FRAME   1
#define SLOT_ACK     2
#define SLOT_CONSUMED 3

/* _rsv[48]: [0..7]=shm_ptr, [8..39]=shm_name, [40..43]=eventfd, [44..47]=last_req_seq */
#define TP_SH_SHM_PTR(rsv)      (*(void **)(rsv))
#define TP_SH_SHM_NAME(rsv)     ((char *)((rsv) + 8))
#define TP_SH_SHM_NAME_SIZE     32
#define TP_SH_EVENTFD(rsv)      (*(int *)((rsv) + 40))
#define TP_SH_LAST_REQ_SEQ(rsv) (*(int *)((rsv) + 44))

void tp_shm_destroy(idk_transport_t *tp);

static inline void *shm_ptr(void *base, int offset) {
    return (char *)base + offset;
}

static inline atomic_int *shm_atom(void *base, int offset) {
    return (atomic_int *)((char *)base + offset);
}

static inline int32_t *shm_i32(void *base, int offset) {
    return (int32_t *)((char *)base + offset);
}

static void tp_shm_health_check(void *ptr) {
    int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
    if (prod_pid > 0 && kill(prod_pid, 0) < 0 && errno == ESRCH)
        atomic_store(shm_atom(ptr, SHM_O_PROD_STATE), -1);
    int cons_pid = *shm_i32(ptr, SHM_O_CONS_PID);
    if (cons_pid > 0 && kill(cons_pid, 0) < 0 && errno == ESRCH)
        atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), -1);
}

#define MAX_SHM_TP 4

static struct {
    idk_transport_t *tp;
    pthread_t        thread;
    atomic_bool      stop;
} g_shm_health[MAX_SHM_TP];

static void *shm_health_loop(void *arg) {
    idk_transport_t *tp = arg;
    while (1) {
        bool stop;
        for (int i = 0; i < MAX_SHM_TP; i++) {
            if (g_shm_health[i].tp != tp) continue;
            stop = atomic_load(&g_shm_health[i].stop);
            break;
        }
        if (stop) break;
        sleep(2);
        void *ptr = TP_SH_SHM_PTR(tp->_rsv);
        if (ptr) tp_shm_health_check(ptr);
    }
    return NULL;
}

static void shm_start_health_thread(idk_transport_t *tp) {
    for (int i = 0; i < MAX_SHM_TP; i++) {
        if (g_shm_health[i].tp) continue;
        g_shm_health[i].tp = tp;
        atomic_store(&g_shm_health[i].stop, false);
        pthread_create(&g_shm_health[i].thread, NULL, shm_health_loop, tp);
        return;
    }
}

static void shm_stop_health_thread(idk_transport_t *tp) {
    for (int i = 0; i < MAX_SHM_TP; i++) {
        if (g_shm_health[i].tp != tp) continue;
        atomic_store(&g_shm_health[i].stop, true);
        pthread_join(g_shm_health[i].thread, NULL);
        g_shm_health[i].tp = NULL;
        return;
    }
}

static void make_shm_name(const char *name, char *buf, size_t max) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    snprintf(buf, max, "/%s", base);
}

static int futex_wait(atomic_int *uaddr, int val, int timeout_ms) {
    struct timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        /* FUTEX_WAIT interprets timeout as a RELATIVE duration (not absolute).
         * Do NOT add clock_gettime - just set the duration directly. */
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    /* NO FUTEX_PRIVATE_FLAG - the futex lives in shared memory mapped by
     * two different processes. PRIVATE only works for process-internal
     * futexes (heap/stack/anonymous mmap). With PRIVATE, futex_wake from
     * the other process won't find this waiter. */
    int op = FUTEX_WAIT;
    return (int)syscall(__NR_futex, uaddr, op, val, tsp, NULL, 0);
}

static int futex_wake(atomic_int *uaddr) {
    return (int)syscall(__NR_futex, uaddr, FUTEX_WAKE, 1,
                        NULL, NULL, 0);
}

static void *shm_setup(const char *name, int *out_fd, int is_creator) {
    int flags = is_creator ? (O_CREAT | O_RDWR) : O_RDWR;
    mode_t mode = is_creator ? 0600 : 0;

    int fd = shm_open(name, flags, mode);
    if (fd < 0) return NULL;

    if (is_creator && ftruncate(fd, SHM_SIZE) < 0) {
        close(fd);
        return NULL;
    }

    void *ptr = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    *out_fd = fd;
    return ptr;
}

static int shm_init_consumer(idk_transport_t *tp, const char *name) {
    char shm_name[64];
    make_shm_name(name, shm_name, sizeof(shm_name));
    size_t namelen = strlen(shm_name);
    if (namelen >= TP_SH_SHM_NAME_SIZE) namelen = TP_SH_SHM_NAME_SIZE - 1;
    memcpy(TP_SH_SHM_NAME(tp->_rsv), shm_name, namelen + 1);

    shm_unlink(shm_name);

    int shm_fd;
    void *ptr = shm_setup(shm_name, &shm_fd, 1);
    if (!ptr) return -1;

    *shm_i32(ptr, SHM_O_MAGIC) = SHM_MAGIC_VAL;
    atomic_store(shm_atom(ptr, SHM_O_PROD_STATE), 0);
    atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 1);
    *shm_i32(ptr, SHM_O_CONS_PID) = (int32_t)getpid();
    *shm_i32(ptr, SHM_O_PROD_PID) = 0;
    atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);
    atomic_store(shm_atom(ptr, SHM_O_FRAME_SEQ), 0);
    *shm_i32(ptr, SHM_O_EVENTFD) = 0;

    tp->_server_fd = shm_fd;
    tp->_client_fd = -1;
    TP_SH_SHM_PTR(tp->_rsv) = ptr;
    tp->ready = false;

    shm_start_health_thread(tp);

    IDK_LOG("tp", "shm: consumer created %s (pid=%d, waiting for producer)\n",
            shm_name, (int)getpid());
    return 0;
}

static int shm_init_producer(idk_transport_t *tp, const char *name) {
    char shm_name[64];
    make_shm_name(name, shm_name, sizeof(shm_name));
    size_t namelen = strlen(shm_name);
    if (namelen >= TP_SH_SHM_NAME_SIZE) namelen = TP_SH_SHM_NAME_SIZE - 1;
    memcpy(TP_SH_SHM_NAME(tp->_rsv), shm_name, namelen + 1);

    int shm_fd;
    void *ptr = NULL;

    for (int i = 0; i < 300; i++) {
        ptr = shm_setup(shm_name, &shm_fd, 0);
        if (ptr) break;
        if (errno == ENOENT || errno == EACCES) {
            usleep(100000);  /* 100ms between retries */
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

    int efd = (int)syscall(__NR_eventfd2, 0, EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0)
        efd = 0;
    *shm_i32(ptr, SHM_O_EVENTFD) = efd;
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
    if (sn[0]) memcpy(shm_name_save, sn, sizeof(shm_name_save) - 1);

    int efd = TP_SH_EVENTFD(tp->_rsv);
    if (efd > 0) { close(efd); TP_SH_EVENTFD(tp->_rsv) = 0; }

    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (ptr) {
        if (tp->role == IDK_TP_CONSUMER) {
            atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), -1);
            futex_wake(shm_atom(ptr, SHM_O_CONS_STATE));
        }
        munmap(ptr, SHM_SIZE);
    }
    if (tp->_server_fd >= 0) { close(tp->_server_fd); tp->_server_fd = -1; }
    if (tp->_client_fd >= 0) { close(tp->_client_fd); tp->_client_fd = -1; }
    tp->ready = false;

    memset(tp->_rsv, 0, sizeof(tp->_rsv));

    if (shm_name_save[0] && tp->role == IDK_TP_CONSUMER)
        shm_unlink(shm_name_save);
}

void tp_shm_disconnect_client(idk_transport_t *tp) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);

    if (tp->role == IDK_TP_CONSUMER && ptr) {
        int efd = TP_SH_EVENTFD(tp->_rsv);
        if (efd > 0) { close(efd); TP_SH_EVENTFD(tp->_rsv) = 0; }

        atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);
        *shm_i32(ptr, SHM_O_PROD_PID) = 0;
        *shm_i32(ptr, SHM_O_DMABUF_NFD) = 0;
        atomic_store(shm_atom(ptr, SHM_O_FRAME_SEQ), 0);
        atomic_store(shm_atom(ptr, SHM_O_REQ_SEQ), 0);

        if (tp->_client_fd >= 0) { close(tp->_client_fd); tp->_client_fd = -1; }

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

int tp_shm_accept(idk_transport_t *tp) {
    if (tp->ready) return 1;

    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr) {
        if (tp->_server_fd < 0 && tp->_client_fd < 0)
            return -1;
        return -1;
    }

    int ps = atomic_load(shm_atom(ptr, SHM_O_PROD_STATE));
    if (ps != 1) {
        if (ps == -1) return -1;
        return 0;
    }

    int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
    if (prod_pid <= 0) return -1;

    int pidfd = (int)syscall(__NR_pidfd_open, prod_pid, 0);
    if (pidfd < 0) {
        IDK_ERR("tp", "shm: pidfd_open(%d) failed: %s\n", prod_pid, strerror(errno));
        return -1;
    }
    tp->_client_fd = pidfd;

    int efd_fd = *shm_i32(ptr, SHM_O_EVENTFD);
    if (efd_fd > 0) {
        int dup_efd = (int)syscall(__NR_pidfd_getfd, pidfd, efd_fd, 0);
        if (dup_efd < 0) {
            IDK_LOG("tp", "shm: pidfd_getfd(eventfd) failed: %s\n", strerror(errno));
            TP_SH_EVENTFD(tp->_rsv) = 0;
        } else {
            TP_SH_EVENTFD(tp->_rsv) = dup_efd;
        }
    }

    atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 2);
    futex_wake(shm_atom(ptr, SHM_O_CONS_STATE));
    tp->ready = true;
    IDK_LOG("tp", "shm: consumer ready, producer pid=%d pidfd=%d\n", prod_pid, pidfd);
    return 1;
}

int tp_shm_poll(idk_transport_t *tp) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr || !tp->ready) return -1;

    if (atomic_load(shm_atom(ptr, SHM_O_PROD_STATE)) == -1) return -1;

    int s = atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE));
    return (s == SLOT_FRAME) ? 1 : 0;
}

int tp_shm_recv(idk_transport_t *tp, idk_frame_header_t *hdr,
                int fds[4], int *nfd) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr || !tp->ready || !hdr || !fds || !nfd) {
        errno = EINVAL;
        return -1;
    }

    if (atomic_load(shm_atom(ptr, SHM_O_PROD_STATE)) == -1) {
        return -1;
    }

    int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
    if (prod_pid > 0 && kill(prod_pid, 0) < 0 && errno == ESRCH) {
        IDK_ERR("tp", "shm: producer pid=%d dead (crashed without graceful shutdown)\n", prod_pid);
        return -1;
    }

    if (atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE)) != SLOT_FRAME) {
        return 0;
    }

    memcpy(hdr, shm_ptr(ptr, SHM_O_HDR), sizeof(*hdr));
    *nfd = *shm_i32(ptr, SHM_O_DMABUF_NFD);
    if (*nfd < 0) *nfd = 0;
    if (*nfd > 4) *nfd = 4;
    for (int i = 0; i < *nfd; i++) {
        int target_fd = shm_i32(ptr, SHM_O_DMABUF_FDS)[i];
        int stolen = (int)syscall(__NR_pidfd_getfd, tp->_client_fd,
                                  target_fd, 0);
        if (stolen < 0) {
            IDK_ERR("tp", "shm: pidfd_getfd(%d) failed: %s\n",
                    target_fd, strerror(errno));
            for (int j = 0; j < i; j++) close(fds[j]);
            *nfd = 0;
            return -1;
        }
        fds[i] = stolen;
    }

    atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_CONSUMED);

    return 1;
}

void tp_shm_send_ack(idk_transport_t *tp, const idk_ack_msg_t *ack) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr || !tp->ready || !ack) return;

    memcpy(shm_ptr(ptr, SHM_O_ACK), ack, sizeof(*ack));
    atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_ACK);
    futex_wake(shm_atom(ptr, SHM_O_SLOT_STATE));
}

int tp_shm_send(idk_transport_t *tp, const idk_frame_header_t *hdr,
                const int *fds, int nfd) {
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

    for (int tries = 0; ; tries++) {
        int s = atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE));
        if (s == SLOT_EMPTY || s == SLOT_ACK) break;
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
        deadline.tv_sec  += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }

    atomic_int *slot = shm_atom(ptr, SHM_O_SLOT_STATE);
    while (1) {
        int s = atomic_load(slot);
        if (s == SLOT_ACK) break;

        if (s == -1 || atomic_load(shm_atom(ptr, SHM_O_CONS_STATE)) == -1) {
            errno = ECONNRESET;
            return -1;
        }
        if (timeout_ms >= 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec)) {
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

int tp_shm_send_request(idk_transport_t *tp, const idk_request_msg_t *req) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr || !tp->ready || !req) { errno = EINVAL; return -1; }
    atomic_int *req_seq = shm_atom(ptr, SHM_O_REQ_SEQ);
    atomic_fetch_add(req_seq, 1);
    atomic_thread_fence(memory_order_release);
    futex_wake(req_seq);
    return 0;
}

int tp_shm_recv_request(idk_transport_t *tp, idk_request_msg_t *req, int timeout_ms) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr || !tp->ready || !req) { errno = EINVAL; return -1; }

    int *last_seq = &TP_SH_LAST_REQ_SEQ(tp->_rsv);
    atomic_int *req_seq = shm_atom(ptr, SHM_O_REQ_SEQ);

    struct timespec deadline;
    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_nsec += (long)timeout_ms * 1000000L;
        deadline.tv_sec  += deadline.tv_nsec / 1000000000L;
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
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec)) {
                errno = ETIMEDOUT;
                return -1;
            }
        }

        futex_wait(req_seq, cur, 100);
    }
}

/* Simplex API: no ACK/REQUEST/fds, event fits in SHM_O_HDR slot */

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
