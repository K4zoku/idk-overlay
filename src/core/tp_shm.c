/* tp_shm.c — SHM + futex + pidfd_getfd transport backend.
 *
 * Implements the idk_transport interface using a shared memory ring
 * buffer with futex synchronization and pidfd_getfd(2) for DMABUF fd
 * passing.  Kernel 5.6+ required (pidfd_getfd).
 *
 * Consumer: creates SHM at /dev/shm/idk-<name>, maps it.
 * Producer: opens existing SHM, writes PID, signals ready.
 * Frame data + ACK pass through the shared page; DMABUF fds are
 * transferred via pidfd_getfd (consumer steals fd numbers written
 * into SHM by the producer).
 */

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
#include <time.h>
#include <stdatomic.h>

#include "core/transport.h"
#include "core/log.h"

/* ── SHM layout (single page) ────────────────────────────────────────── */

#define SHM_SIZE 4096

/* Offsets within the shared page */
#define SHM_O_MAGIC       0   /* uint32_t   — magic value */
#define SHM_O_PROD_STATE  4   /* atomic_int — 0=init,1=ready,-1=dead */
#define SHM_O_CONS_STATE  8   /* atomic_int — 0=init,1=ready,-1=dead */
#define SHM_O_PROD_PID    12  /* int32_t    — producer PID (for consumer pidfd) */
#define SHM_O_CONS_PID    16  /* int32_t    — consumer PID (for producer liveness check) */
#define SHM_O_DMABUF_NFD  20  /* int32_t    — # of dmabuf fds (0-4) */
#define SHM_O_DMABUF_FDS  24  /* int32_t[4] — fd numbers in producer */
#define SHM_O_HDR         40  /* idk_frame_header_t (28 bytes) */
#define SHM_O_ACK         68  /* idk_ack_msg_t (12 bytes) */
#define SHM_O_SLOT_STATE  80  /* atomic_int — 0=empty,1=frame,2=ack */
#define SHM_O_FRAME_SEQ   84  /* atomic_int — incremented each frame sent */
#define SHM_O_REQ_SEQ     88  /* atomic_int — request sequence number     */

_Static_assert(SHM_O_REQ_SEQ + 4 <= SHM_SIZE,
               "SHM layout exceeds page size");

#define SHM_MAGIC_VAL 0x4D485349  /* "SHMI" */

#define SLOT_EMPTY   0
#define SLOT_FRAME   1
#define SLOT_ACK     2
#define SLOT_CONSUMED 3

/* ── _rsv[48] layout for SHM backend ─────────────────────────────────── */
/*  [ 0..7 ]  void *shm_ptr       — mmap'd SHM address
 *  [ 8..43 ]  char  shm_name[36]  — SHM name (for reinit/lookup)
 *  [44..47]  int   last_req_seq   — last seen REQUEST sequence         */

#define TP_SH_SHM_PTR(rsv)      (*(void **)(rsv))
#define TP_SH_SHM_NAME(rsv)     ((char *)((rsv) + 8))
#define TP_SH_LAST_REQ_SEQ(rsv) (*(int *)((rsv) + 44))

/* Forward declarations (defined later, needed by init error paths) */
void tp_shm_destroy(idk_transport_t *tp);

/* ── Helpers ─────────────────────────────────────────────────────────── */

static inline atomic_int *shm_atom(void *base, int offset) {
    return (atomic_int *)((char *)base + offset);
}

static inline int32_t *shm_i32(void *base, int offset) {
    return (int32_t *)((char *)base + offset);
}

static inline void *shm_ptr(void *base, int offset) {
    return (char *)base + offset;
}

/* Build an SHM name from a transport name (e.g. "/tmp/idk-overlay-1234"
 * → "/idk-overlay-1234").
 * Writes at most max-1 chars + NUL into buf. */
static void make_shm_name(const char *name, char *buf, size_t max) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    snprintf(buf, max, "/%s", base);
}

static int futex_wait(atomic_int *uaddr, int val, int timeout_ms) {
    struct timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        /* FUTEX_WAIT interprets timeout as a RELATIVE duration (not absolute).
         * Do NOT add clock_gettime — just set the duration directly. */
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    /* NO FUTEX_PRIVATE_FLAG — the futex lives in shared memory mapped by
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

/* Create or open SHM, mmap it, return mapped address or NULL. */
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

/* ── Consumer init — NON-BLOCKING ──────────────────────────────────────
 *
 * Creates SHM, writes consumer PID, sets CONS_STATE=1, returns immediately.
 * Does NOT wait for producer — that happens lazily in tp_shm_accept
 * (called every frame from compositor render loop). This avoids blocking
 * the hook install thread, which would race with the game's main thread
 * if install_addr (trampoline code patching) runs after a long init. */

static int shm_init_consumer(idk_transport_t *tp, const char *name) {
    char shm_name[64];
    make_shm_name(name, shm_name, sizeof(shm_name));
    memcpy(TP_SH_SHM_NAME(tp->_rsv), shm_name, strlen(shm_name) + 1);

    /* Clean up any stale SHM from a previous crash */
    shm_unlink(shm_name);

    int shm_fd;
    void *ptr = shm_setup(shm_name, &shm_fd, 1);
    if (!ptr) return -1;

    /* Initialize SHM header */
    *shm_i32(ptr, SHM_O_MAGIC) = SHM_MAGIC_VAL;
    atomic_store(shm_atom(ptr, SHM_O_PROD_STATE), 0);
    atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 1);  /* consumer partially ready */
    *shm_i32(ptr, SHM_O_CONS_PID) = (int32_t)getpid();
    *shm_i32(ptr, SHM_O_PROD_PID) = 0;
    atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);
    atomic_store(shm_atom(ptr, SHM_O_FRAME_SEQ), 0);

    tp->_server_fd = shm_fd;
    tp->_client_fd = -1;  /* pidfd opened lazily in accept */
    TP_SH_SHM_PTR(tp->_rsv) = ptr;
    tp->ready = false;  /* not fully ready until producer connects + pidfd opened */

    IDK_LOG("tp", "shm: consumer created %s (pid=%d, waiting for producer)\n",
            shm_name, (int)getpid());
    return 0;
}

/* ── Producer init ────────────────────────────────────────────────────── */

static int shm_init_producer(idk_transport_t *tp, const char *name) {
    char shm_name[64];
    make_shm_name(name, shm_name, sizeof(shm_name));
    memcpy(TP_SH_SHM_NAME(tp->_rsv), shm_name, strlen(shm_name) + 1);

    int shm_fd;
    void *ptr = NULL;

    /* Retry loop: wait for consumer to create SHM */
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

    /* Verify magic */
    if (*shm_i32(ptr, SHM_O_MAGIC) != SHM_MAGIC_VAL) {
        IDK_ERR("tp", "shm: bad magic on %s\n", shm_name);
        close(shm_fd);
        return -1;
    }

    /* Write PID and signal ready */
    *shm_i32(ptr, SHM_O_PROD_PID) = (int32_t)getpid();
    atomic_store(shm_atom(ptr, SHM_O_PROD_STATE), 1);
    futex_wake(shm_atom(ptr, SHM_O_PROD_STATE));

    tp->_server_fd = -1;
    tp->_client_fd = shm_fd;
    TP_SH_SHM_PTR(tp->_rsv) = ptr;

    /* Wait for consumer to be fully ready (pidfd opened) before
     * declaring ready. Consumer's tp_shm_accept (called every frame)
     * detects PROD_STATE==1, opens pidfd, sets CONS_STATE=2. */
    IDK_LOG("tp", "shm: producer waiting for consumer on %s\n", shm_name);
    atomic_int *cons_state = shm_atom(ptr, SHM_O_CONS_STATE);
    int waited_ms = 0;
    while (atomic_load(cons_state) != 2) {
        if (atomic_load(cons_state) == -1) {
            IDK_ERR("tp", "shm: consumer died before ready on %s\n", shm_name);
            close(shm_fd);
            return -1;
        }
        /* futex_wait(addr, val, timeout) only blocks if *addr == val.
         * If cons_state is 0 (consumer created SHM but hasn't set
         * CONS_STATE=1 yet — shouldn't happen per current code, but
         * defensively), futex_wait(cons_state, 1, ...) returns
         * immediately with EAGAIN and the loop would busy-spin,
         * burning CPU and racing the 30s timeout accumulator (which
         * increments by 1000 per spin). Use the current value as the
         * futex wait condition so we block regardless of state. */
        int cur_state = atomic_load(cons_state);
        int rc = futex_wait(cons_state, cur_state, 1000);
        (void)rc;
        waited_ms += 1000;
        if (waited_ms >= 30000) {
            IDK_ERR("tp", "shm: timeout waiting for consumer on %s\n", shm_name);
            close(shm_fd);
            return -1;
        }
    }

    tp->ready = true;
    int cons_pid = *shm_i32(ptr, SHM_O_CONS_PID);
    IDK_LOG("tp", "shm: producer ready on %s (consumer pid=%d)\n", shm_name, cons_pid);
    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

int tp_shm_init(idk_transport_t *tp, const char *name) {
    if (tp->role == IDK_TP_CONSUMER)
        return shm_init_consumer(tp, name);
    else
        return shm_init_producer(tp, name);
}

void tp_shm_destroy(idk_transport_t *tp) {
    /* Save SHM name before we clear _rsv */
    char shm_name_save[40] = {0};
    const char *sn = TP_SH_SHM_NAME(tp->_rsv);
    if (sn[0]) memcpy(shm_name_save, sn, sizeof(shm_name_save) - 1);

    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (ptr) {
        if (tp->role == IDK_TP_CONSUMER) {
            /* Signal producer we're gone */
            atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), -1);
            futex_wake(shm_atom(ptr, SHM_O_CONS_STATE));
        }
        munmap(ptr, SHM_SIZE);
    }
    if (tp->_server_fd >= 0) { close(tp->_server_fd); tp->_server_fd = -1; }
    if (tp->_client_fd >= 0) { close(tp->_client_fd); tp->_client_fd = -1; }
    tp->ready = false;

    /* Clear _rsv so dangling pointer is never accessible again */
    memset(tp->_rsv, 0, sizeof(tp->_rsv));

    if (shm_name_save[0] && tp->role == IDK_TP_CONSUMER)
        shm_unlink(shm_name_save);
}

/* Soft-disconnect — used by the compositor's recv-failure path so a
 * restarted webview can reconnect on a subsequent frame.
 *
 * SHM consumer: closes the pidfd (_client_fd), resets the producer
 * half of the SHM header (PROD_STATE=0, PROD_PID=0, SLOT=EMPTY) and
 * re-arms CONS_STATE=1 (partially ready). The SHM mapping, shm_fd,
 * and shm name are all preserved so a new producer can shm_open the
 * same name, write its PID, and the consumer's tp_shm_accept on the
 * next frame picks it up.
 *
 * SHM producer (webview side): no server state to keep → full destroy. */
void tp_shm_disconnect_client(idk_transport_t *tp) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);

    if (tp->role == IDK_TP_CONSUMER && ptr) {
        /* Tear down producer-side state so a new producer can take over.
         * Order matters: clear PROD_PID and SLOT first, then set
         * PROD_STATE=0 last so a producer polling for "ready" doesn't
         * see a half-reset state. */
        atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);
        *shm_i32(ptr, SHM_O_PROD_PID) = 0;
        *shm_i32(ptr, SHM_O_DMABUF_NFD) = 0;
        atomic_store(shm_atom(ptr, SHM_O_FRAME_SEQ), 0);
        atomic_store(shm_atom(ptr, SHM_O_REQ_SEQ), 0);

        /* Close pidfd — new producer will have a different PID. */
        if (tp->_client_fd >= 0) { close(tp->_client_fd); tp->_client_fd = -1; }

        /* Re-arm consumer "partially ready" so a new producer's
         * tp_shm_accept path detects us. Keep CONS_PID so producer's
         * liveness check still works. */
        atomic_store(shm_atom(ptr, SHM_O_PROD_STATE), 0);
        atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 1);
        futex_wake(shm_atom(ptr, SHM_O_CONS_STATE));

        IDK_LOG("tp", "shm: consumer soft-disconnected (pidfd closed, SHM kept open for reconnect)\n");
    } else if (tp->role == IDK_TP_PRODUCER) {
        /* Producer has no server fd — full destroy is the only sane option. */
        tp_shm_destroy(tp);
        return;
    }

    tp->ready = false;
}

/* ── Consumer API ─────────────────────────────────────────────────────── */

/* Called every frame from compositor render loop. Non-blocking.
 * If not yet ready, checks if producer connected; if so, opens pidfd,
 * signals CONS_STATE=2, sets ready. */
int tp_shm_accept(idk_transport_t *tp) {
    if (tp->ready) return 1;

    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr) {
        /* _rsv cleared → transport was destroyed or never initialized */
        if (tp->_server_fd < 0 && tp->_client_fd < 0)
            return -1;
        return -1;
    }

    /* Check if producer has connected */
    int ps = atomic_load(shm_atom(ptr, SHM_O_PROD_STATE));
    if (ps != 1) {
        if (ps == -1) return -1;  /* producer died */
        return 0;  /* still waiting */
    }

    /* Producer connected — open pidfd */
    int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
    if (prod_pid <= 0) return -1;

    int pidfd = (int)syscall(__NR_pidfd_open, prod_pid, 0);
    if (pidfd < 0) {
        IDK_ERR("tp", "shm: pidfd_open(%d) failed: %s\n", prod_pid, strerror(errno));
        return -1;
    }
    tp->_client_fd = pidfd;

    /* Signal fully ready — producer waits for this */
    atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 2);
    futex_wake(shm_atom(ptr, SHM_O_CONS_STATE));
    tp->ready = true;
    IDK_LOG("tp", "shm: consumer ready, producer pid=%d pidfd=%d\n", prod_pid, pidfd);
    return 1;
}

int tp_shm_poll(idk_transport_t *tp) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr || !tp->ready) return -1;

    /* Check producer still alive */
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

    /* Check producer is still alive (graceful shutdown path). */
    if (atomic_load(shm_atom(ptr, SHM_O_PROD_STATE)) == -1) {
        return -1;
    }

    /* Out-of-band liveness check: producer may have crashed (SIGKILL,
     * segfault) without setting PROD_STATE=-1. The producer-side
     * tp_shm_send checks consumer liveness via kill(cons_pid, 0) —
     * mirror that here. Without this, a crashed webview leaves
     * PROD_STATE=1 and the compositor polls forever, freezing the
     * overlay on the last frame. */
    int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
    if (prod_pid > 0 && kill(prod_pid, 0) < 0 && errno == ESRCH) {
        IDK_ERR("tp", "shm: producer pid=%d dead (crashed without graceful shutdown)\n", prod_pid);
        return -1;
    }

    /* No frame available */
    if (atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE)) != SLOT_FRAME) {
        return 0;
    }

    /* Read header */
    memcpy(hdr, shm_ptr(ptr, SHM_O_HDR), sizeof(*hdr));

    /* Steal dmabuf fds via pidfd_getfd */
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

    /* Mark slot consumed — prevents re-reading the same frame before
     * tp_shm_send_ack transitions to SLOT_ACK. The producer's
     * tp_shm_send checks for SLOT_EMPTY || SLOT_ACK, so SLOT_CONSUMED
     * keeps it from overwriting until we're done. */
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

/* ── Producer API ─────────────────────────────────────────────────────── */

int tp_shm_send(idk_transport_t *tp, const idk_frame_header_t *hdr,
                const int *fds, int nfd) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr || !tp->ready || !hdr || !fds || nfd < 1 || nfd > 4) {
        errno = EINVAL;
        return -1;
    }

    /* Check consumer still alive via kill(pid, 0).
     * On consumer segfault, tp_shm_destroy is NOT called, so CONS_STATE
     * stays at 2. We need an out-of-band liveness check. */
    int cons_pid = *shm_i32(ptr, SHM_O_CONS_PID);
    if (cons_pid > 0 && kill(cons_pid, 0) < 0 && errno == ESRCH) {
        IDK_ERR("tp", "shm: consumer pid=%d dead\n", cons_pid);
        errno = ECONNRESET;
        return -1;
    }

    /* Also check CONS_STATE for graceful shutdown */
    if (atomic_load(shm_atom(ptr, SHM_O_CONS_STATE)) == -1) {
        errno = ECONNRESET;
        return -1;
    }

    /* Wait for slot to be empty (consumer must have read previous ack).
     * Unlike the old code which returned EAGAIN immediately, we wait
     * (with futex + small timeout) so the caller doesn't have to busy-
     * loop retry — the webview's tryExportDMABufOpenGL retries are
     * tight and can burn CPU for seconds. */
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

    /* Write dmabuf fd numbers into SHM (these are fd numbers in OUR
     * process — consumer steals them via pidfd_getfd) */
    *shm_i32(ptr, SHM_O_DMABUF_NFD) = nfd;
    for (int i = 0; i < nfd; i++)
        shm_i32(ptr, SHM_O_DMABUF_FDS)[i] = fds[i];

    /* Write header */
    memcpy(shm_ptr(ptr, SHM_O_HDR), hdr, sizeof(*hdr));

    /* Increment frame sequence for liveness tracking */
    atomic_fetch_add(shm_atom(ptr, SHM_O_FRAME_SEQ), 1);

    /* Memory barrier: ensure header + fds visible before slot state */
    atomic_thread_fence(memory_order_release);

    /* Signal consumer */
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

    /* Compute absolute deadline for real timeout tracking */
    struct timespec deadline;
    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_nsec += (long)timeout_ms * 1000000L;
        deadline.tv_sec  += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }

    /* Wait for slot to become SLOT_ACK */
    atomic_int *slot = shm_atom(ptr, SHM_O_SLOT_STATE);
    while (1) {
        int s = atomic_load(slot);
        if (s == SLOT_ACK) break;

        /* Check consumer dead */
        if (s == -1 || atomic_load(shm_atom(ptr, SHM_O_CONS_STATE)) == -1) {
            errno = ECONNRESET;
            return -1;
        }

        /* Real elapsed-time check */
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

        /* futex_wait on slot while value == s — wakes when consumer
         * changes it (to SLOT_CONSUMED or SLOT_ACK).
         * Using the current slot value means we correctly block through
         * SLOT_FRAME → SLOT_CONSUMED → SLOT_ACK transitions without
         * busy-looping or premature timeouts. */
        int rc = futex_wait(slot, s, 100);
        (void)rc;
    }

    /* Read ack and reset slot */
    atomic_thread_fence(memory_order_acquire);
    memcpy(ack, shm_ptr(ptr, SHM_O_ACK), sizeof(*ack));
    atomic_store(slot, SLOT_EMPTY);
    futex_wake(slot);  /* wake producer in case it's waiting for empty */

    return 0;
}

/* ── REQUEST support ────────────────────────────────────────────────────── */

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
