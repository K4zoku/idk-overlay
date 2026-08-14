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
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "internal.h"

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  char name[64];
  make_broker_name(name, sizeof(name));
  int srv = bind_abstract(name);
  if (srv < 0) {
    fprintf(stderr, "idk-broker: bind abstract \\0%s failed: %s\n", name + 1, strerror(errno));
    return 1;
  }
  fprintf(stderr, "idk-broker: listening on abstract \\0%s\n", name + 1);

  for (;;) {
    int cfd = accept(srv, NULL, NULL);
    if (cfd < 0) {
      if (errno == EINTR)
        continue;
      perror("accept");
      continue;
    }
    struct session_arg *sa = malloc(sizeof(*sa));
    if (!sa) {
      close(cfd);
      continue;
    }
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
