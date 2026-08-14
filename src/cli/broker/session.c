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

  /* Wait briefly for exec confirmation. A successful exec closes
   * the pipe end (CLOEXEC) → read returns 0 (EOF). Failure writes
   * an errno int before _exit(127). */
  struct pollfd pfd = {.fd = exec_err_fd, .events = POLLIN};
  int erc = poll(&pfd, 1, 5000);
  if (erc < 0) {
    send_ack(cfd, ACK_ERROR);
    reap_webview(wv);
    close(exec_err_fd);
    return;
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
  fprintf(stderr, "idk-broker: webview spawned pid=%d (comm=%s tp=%s input=%s)\n", (int)wv, hs.comm, hs.tp_socket,
          hs.input_socket);

  for (;;) {
    struct pollfd pf = {.fd = cfd, .events = POLLIN | POLLHUP | POLLERR};
    if (poll(&pf, 1, 60000) <= 0) {
      if (pf.revents & (POLLHUP | POLLERR | POLLNVAL))
        break;
      continue;
    }
    char tmp[16];
    ssize_t n = read(cfd, tmp, sizeof(tmp));
    if (n <= 0)
      break;
  }

  reap_webview(wv);
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
