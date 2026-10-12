#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "internal.h"

static int send_ack(int fd, uint8_t code) { return (write(fd, &code, 1) == 1) ? 0 : -1; }

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
  if (validate_handshake(&hs) != 0) {
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
  int pidfd = broker_pidfd_open(wv);
  if (pidfd < 0 && errno != ENOSYS && errno != EINVAL && errno != EPERM)
    fprintf(stderr, "idk-broker: pidfd_open failed: %s\n", strerror(errno));

  /* A successful exec closes the CLOEXEC pipe; failure writes errno. */
  struct pollfd startup[2] = {
      {.fd = exec_err_fd, .events = POLLIN | POLLHUP, .revents = 0},
      {.fd = pidfd, .events = POLLIN | POLLHUP, .revents = 0},
  };
  int erc;
  do {
    erc = poll(startup, pidfd >= 0 ? 2 : 1, 5000);
  } while (erc < 0 && errno == EINTR);
  if (erc <= 0) {
    send_ack(cfd, ACK_ERROR);
    reap_webview(wv, pidfd);
    close(exec_err_fd);
    return;
  }
  if (startup[0].revents & (POLLIN | POLLHUP)) {
    int errc = 0;
    ssize_t n = read(exec_err_fd, &errc, sizeof(errc));
    if (n > 0 || (pidfd >= 0 && (startup[1].revents & (POLLIN | POLLHUP)))) {
      if (n > 0)
        fprintf(stderr, "idk-broker: webview exec failed: %s\n", strerror(errc));
      send_ack(cfd, ACK_ERROR);
      reap_webview(wv, pidfd);
      close(exec_err_fd);
      return;
    }
  } else if (pidfd >= 0 && (startup[1].revents & (POLLIN | POLLHUP))) {
    send_ack(cfd, ACK_ERROR);
    reap_webview(wv, pidfd);
    close(exec_err_fd);
    return;
  }
  close(exec_err_fd);

  send_ack(cfd, ACK_OK);
  fprintf(stderr, "idk-broker: webview spawned pid=%d (comm=%s tp=%s input=%s)\n", (int)wv, hs.comm, hs.tp_socket,
          hs.input_socket);

  for (;;) {
    struct pollfd pfds[2] = {
        {.fd = cfd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0},
        {.fd = pidfd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0},
    };
    int nfds = pidfd >= 0 ? 2 : 1;
    int rc;
    do {
      rc = poll(pfds, nfds, 60000);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
      break;
    if (pidfd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      reap_webview(wv, pidfd);
      pidfd = -1;
      break;
    }
    if (rc == 0)
      continue;
    if (pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
      break;
    char tmp[16];
    ssize_t n = read(cfd, tmp, sizeof(tmp));
    if (n <= 0)
      break;
  }

  if (pidfd >= 0)
    reap_webview(wv, pidfd);
  fprintf(stderr, "idk-broker: session end (webview pid=%d)\n", (int)wv);
}

void *session_thread(void *arg) {
  struct session_arg *sa = (struct session_arg *)arg;
  int cfd = sa->cfd;
  free(sa);
  handle_session(cfd);
  close(cfd);
  return NULL;
}
