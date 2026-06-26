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
#define SHM_O_PROD_PID    12  /* int32_t    — producer PID (for pidfd) */
#define SHM_O_DMABUF_NFD  16  /* int32_t    — # of dmabuf fds (0-4) */
#define SHM_O_DMABUF_FDS  20  /* int32_t[4] — fd numbers in producer */
#define SHM_O_HDR         36  /* idk_frame_header_t (28 bytes) */
#define SHM_O_ACK         64  /* idk_ack_msg_t (12 bytes) */
#define SHM_O_SLOT_STATE  76  /* atomic_int — 0=empty,1=frame,2=ack */

_Static_assert(SHM_O_SLOT_STATE + 4 <= SHM_SIZE,
               "SHM layout exceeds page size");

#define SHM_MAGIC_VAL 0x4D485349  /* "SHMI" */

#define SLOT_EMPTY 0
#define SLOT_FRAME 1
#define SLOT_ACK   2

/* ── _rsv[48] layout for SHM backend ─────────────────────────────────── */
/*  [ 0..7 ]  void *shm_ptr     — mmap'd SHM address
 *  [ 8..47 ]  char  shm_name[40] — SHM name (for reinit/lookup)      */

#define TP_SH_SHM_PTR(rsv)  (*(void **)(rsv))
#define TP_SH_SHM_NAME(rsv) ((char *)((rsv) + 8))

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
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        tsp = &ts;
    }
    int op = FUTEX_WAIT | FUTEX_PRIVATE_FLAG;
    return (int)syscall(__NR_futex, uaddr, op, val, tsp, NULL, 0);
}

static int futex_wake(atomic_int *uaddr) {
    return (int)syscall(__NR_futex, uaddr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1,
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

/* ── Consumer init ───────────────────────────────────────────────────── */

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
    atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 1);  /* consumer ready */
    atomic_store(shm_atom(ptr, SHM_O_SLOT_STATE), SLOT_EMPTY);

    tp->_server_fd = shm_fd;
    tp->_client_fd = -1;
    TP_SH_SHM_PTR(tp->_rsv) = ptr;
    tp->ready = false;

    /* Wait for producer to signal ready */
    IDK_LOG("tp", "shm: consumer waiting for producer on %s\n", shm_name);
    int waited = 0;
    while (atomic_load(shm_atom(ptr, SHM_O_PROD_STATE)) != 1) {
        if (waited >= 0 && futex_wait(shm_atom(ptr, SHM_O_PROD_STATE), 0, 30000) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        }
        if (++waited > 600) {  /* ~30s total */
            IDK_ERR("tp", "shm: timeout waiting for producer on %s\n", shm_name);
            tp_shm_destroy(tp);
            return -1;
        }
        usleep(50000);
    }

    /* Open pidfd for producer */
    int prod_pid = *shm_i32(ptr, SHM_O_PROD_PID);
    int pidfd = (int)syscall(__NR_pidfd_open, prod_pid, 0);
    if (pidfd < 0) {
        IDK_ERR("tp", "shm: pidfd_open(%d) failed: %s\n", prod_pid, strerror(errno));
        tp_shm_destroy(tp);
        return -1;
    }
    tp->_client_fd = pidfd;

    atomic_store(shm_atom(ptr, SHM_O_CONS_STATE), 2);  /* consumer fully ready */
    tp->ready = true;
    IDK_LOG("tp", "shm: consumer ready, producer pid=%d pidfd=%d\n", prod_pid, pidfd);
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
    tp->ready = true;

    IDK_LOG("tp", "shm: producer ready on %s\n", shm_name);
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

    const char *shm_name = TP_SH_SHM_NAME(tp->_rsv);
    if (shm_name[0] && tp->role == IDK_TP_CONSUMER)
        shm_unlink(shm_name);
}

/* ── Consumer API ─────────────────────────────────────────────────────── */

int tp_shm_accept(idk_transport_t *tp) {
    (void)tp;
    /* In SHM mode, accept is implicit during init — producer connects
     * by opening the SHM and writing its PID. */
    return tp->ready ? 1 : 0;
}

int tp_shm_poll(idk_transport_t *tp) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr || !tp->ready) return -1;

    int s = atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE));
    if (s == SLOT_FRAME) return 1;
    if (s == SLOT_EMPTY) return 0;
    return 0;  /* SLOT_ACK means consumer-side not yet read */
}

int tp_shm_recv(idk_transport_t *tp, idk_frame_header_t *hdr,
                int fds[4], int *nfd) {
    void *ptr = TP_SH_SHM_PTR(tp->_rsv);
    if (!ptr || !tp->ready || !hdr || !fds || !nfd) {
        errno = EINVAL;
        return -1;
    }

    /* Wait for producer to write a frame */
    if (tp_shm_poll(tp) != 1) {
        if (atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE)) == SLOT_EMPTY)
            return 0;  /* no data yet */
        /* else: protocol violation */
        return -1;
    }

    /* Read header */
    memcpy(hdr, shm_ptr(ptr, SHM_O_HDR), sizeof(*hdr));

    /* Steal dmabuf fds via pidfd_getfd */
    *nfd = *shm_i32(ptr, SHM_O_DMABUF_NFD);
    if (*nfd > 4) *nfd = 4;
    for (int i = 0; i < *nfd; i++) {
        int target_fd = shm_i32(ptr, SHM_O_DMABUF_FDS)[i];
        int stolen = (int)syscall(__NR_pidfd_getfd, tp->_client_fd,
                                  target_fd, 0);
        if (stolen < 0) {
            /* Close any fds already stolen */
            for (int j = 0; j < i; j++) close(fds[j]);
            *nfd = 0;
            return -1;
        }
        fds[i] = stolen;
    }

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

    /* Wait for slot to be empty (consumer must have read previous ack) */
    int s = atomic_load(shm_atom(ptr, SHM_O_SLOT_STATE));
    if (s != SLOT_EMPTY && s != SLOT_ACK) {
        errno = EAGAIN;
        return -1;
    }

    /* Write dmabuf fd numbers into SHM */
    *shm_i32(ptr, SHM_O_DMABUF_NFD) = nfd;
    for (int i = 0; i < nfd; i++)
        shm_i32(ptr, SHM_O_DMABUF_FDS)[i] = fds[i];

    /* Write header */
    memcpy(shm_ptr(ptr, SHM_O_HDR), hdr, sizeof(*hdr));

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

    /* Wait for slot to become SLOT_ACK */
    atomic_int *slot = shm_atom(ptr, SHM_O_SLOT_STATE);
    int waited = 0;
    while (atomic_load(slot) != SLOT_ACK) {
        if (timeout_ms >= 0 && waited >= timeout_ms) { errno = ETIMEDOUT; return -1; }
        if (futex_wait(slot, SLOT_FRAME, timeout_ms) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == ETIMEDOUT) return -1;
        }
        waited += 10;  /* approximate; real timeout via futex */
    }

    /* Read ack and reset slot */
    memcpy(ack, shm_ptr(ptr, SHM_O_ACK), sizeof(*ack));
    atomic_store(slot, SLOT_EMPTY);

    return 0;
}
