/* idk-broker - host-namespace webview spawner for the Wine mount-namespace
 * case.
 *
 * The injection library (libidk-overlay.so), when it detects Wine or
 * IDK_BROKER=1, connects here instead of forking the webview directly.
 * It sends an idk_cp_handshake_t over an abstract AF_UNIX socket; the
 * broker execs idk-webview in the host mount namespace so the webview
 * gets full filesystem access (Qt .pak, icu, sandbox helpers). The
 * webview then connects DIRECTLY to the overlay's transport/input
 * abstract sockets (SCM_RIGHTS for fd passing) — the broker is not on
 * the hot path. When the game exits, the overlay socket sees EOF and
 * the broker kills the webview child.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "public/idk_ipc.h"

#define ACK_OK    0x00
#define ACK_ERROR 0x01

static void make_broker_name(char *buf, size_t bufsz) {
    buf[0] = '\0';
    snprintf(buf + 1, bufsz - 1, "idk_broker_%d", (int)getuid());
}

static socklen_t abstract_addrlen(const char *name) {
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name + 1));
}

static int bind_abstract(const char *name) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    memcpy(addr.sun_path, name, 1 + strlen(name + 1));
    if (bind(fd, (struct sockaddr *)&addr, abstract_addrlen(name)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    return fd;
}

static bool peercred_ok(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) return false;
    return cred.uid == getuid();
}

static ssize_t recv_full(int fd, void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char *)buf + total, len - total);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* fork+exec idk-webview; returns child pid, or -1 on failure.
 * exec_err_pipe[1] is set CLOEXEC and the parent reads its end to
 * detect exec failure (write on failure → 0 bytes on success). */
static pid_t spawn_webview(const idk_cp_handshake_t *hs, int *exec_err_fd) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) return -1;
    *exec_err_fd = pipefd[0];

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        close(pipefd[0]);
        extern char **environ;
        setenv("IDK_TP_BACKEND", "socket", 1);
        char buf[80];
        snprintf(buf, sizeof(buf), "%s", hs->tp_socket);
        setenv("IDK_TP_ABSTRACT", buf, 1);
        snprintf(buf, sizeof(buf), "%s", hs->input_socket);
        setenv("IDK_INPUT_ABSTRACT", buf, 1);
        setenv("IDK_MATCH", hs->comm, 1);
        char *argv[] = {(char *)"idk-webview", NULL};
        execvp("idk-webview", argv);
        /* exec failed — tell parent */
        int err = errno;
        (void)!write(pipefd[1], &err, sizeof(err));
        _exit(127);
    }
    close(pipefd[1]);
    return pid;
}

static int send_ack(int fd, uint8_t code) {
    return (write(fd, &code, 1) == 1) ? 0 : -1;
}

static void reap_webview(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) != 0) return;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);
}

static void handle_session(int cfd) {
    if (!peercred_ok(cfd)) {
        fprintf(stderr, "idk-broker: peer cred mismatch, dropping\n");
        return;
    }
    idk_cp_handshake_t hs;
    if (recv_full(cfd, &hs, sizeof(hs)) != (ssize_t)sizeof(hs)) {
        fprintf(stderr, "idk-broker: short handshake read\n");
        return;
    }
    if (hs.identity != IDK_CP_ID_OVERLAY) {
        fprintf(stderr, "idk-broker: bad identity 0x%x\n", hs.identity);
        send_ack(cfd, ACK_ERROR);
        return;
    }
    if (hs.tp_socket[0] == '\0' || hs.input_socket[0] == '\0') {
        fprintf(stderr, "idk-broker: empty socket names\n");
        send_ack(cfd, ACK_ERROR);
        return;
    }

    int exec_err_fd = -1;
    pid_t wv = spawn_webview(&hs, &exec_err_fd);
    if (wv < 0) {
        fprintf(stderr, "idk-broker: spawn_webview failed: %s\n", strerror(errno));
        send_ack(cfd, ACK_ERROR);
        return;
    }

    /* Wait briefly for exec confirmation. A successful exec closes
     * the pipe end (CLOEXEC) → read returns 0 (EOF). Failure writes
     * an errno int before _exit(127). */
    struct pollfd pfd = { .fd = exec_err_fd, .events = POLLIN };
    int erc = poll(&pfd, 1, 5000);
    if (erc < 0) {
        send_ack(cfd, ACK_ERROR);
        reap_webview(wv); close(exec_err_fd); return;
    }
    if (erc == 1 && (pfd.revents & POLLIN)) {
        int errc = 0;
        if (read(exec_err_fd, &errc, sizeof(errc)) > 0)
            fprintf(stderr, "idk-broker: webview exec failed: %s\n", strerror(errc));
        send_ack(cfd, ACK_ERROR);
        reap_webview(wv);
        close(exec_err_fd);
        return;
    }
    close(exec_err_fd);

    send_ack(cfd, ACK_OK);
    fprintf(stderr, "idk-broker: webview spawned pid=%d (comm=%s tp=%s input=%s)\n",
            (int)wv, hs.comm, hs.tp_socket, hs.input_socket);

    /* Block until the overlay closes the control socket (game exit). */
    for (;;) {
        struct pollfd pf = { .fd = cfd, .events = POLLIN | POLLHUP | POLLERR };
        if (poll(&pf, 1, 60000) <= 0) {
            if (pf.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
            continue;
        }
        char tmp[16];
        ssize_t n = read(cfd, tmp, sizeof(tmp));
        if (n <= 0) break;
    }

    reap_webview(wv);
    fprintf(stderr, "idk-broker: session end (webview pid=%d)\n", (int)wv);
}

struct session_arg {
    int cfd;
};

static void *session_thread(void *arg) {
    struct session_arg *sa = (struct session_arg *)arg;
    int cfd = sa->cfd;
    free(sa);
    handle_session(cfd);
    close(cfd);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    char name[64];
    make_broker_name(name, sizeof(name));
    int srv = bind_abstract(name);
    if (srv < 0) {
        fprintf(stderr, "idk-broker: bind abstract \\0%s failed: %s\n",
                name + 1, strerror(errno));
        return 1;
    }
    fprintf(stderr, "idk-broker: listening on abstract \\0%s\n", name + 1);

    for (;;) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        struct session_arg *sa = malloc(sizeof(*sa));
        if (!sa) { close(cfd); continue; }
        sa->cfd = cfd;
        pthread_t t;
        if (pthread_create(&t, NULL, session_thread, sa) != 0) {
            free(sa);
            close(cfd);
            continue;
        }
        pthread_detach(t);
    }
    return 0;
}