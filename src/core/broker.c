/* Shared broker + wine detection logic.
 *
 * Used by both the LD_PRELOAD overlay (libidk-overlay.so) and the Vulkan
 * layer (libidk-vklayer.so). Keeping this in one translation unit avoids
 * duplicate copies of the detection logic and the broker socket state. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/broker.h"
#include "core/log.h"
#include "public/idk_ipc.h"

int g_wine_detected = -1;
int g_use_broker = 0;
int g_broker_fd = -1;

int idk_is_wine(void) {
  detect_wine();
  return g_wine_detected == 1;
}

static void make_broker_name(char *buf, size_t bufsz) {
  buf[0] = '\0';
  snprintf(buf + 1, bufsz - 1, "idk_broker_%d", (int)getuid());
}

static socklen_t abstract_addrlen(const char *name) {
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name + 1));
}

bool detect_wine(void) {
  if (g_wine_detected >= 0)
    return g_wine_detected == 1;

  const char *name = idk_process_name();
  size_t len = name ? strlen(name) : 0;
  if (len > 4 && strcasecmp(name + len - 4, ".exe") == 0) {
    g_wine_detected = 1;
    return true;
  }

  /* /proc/self/maps: a single pass looking for Wine's loader or its
   * runtime libs. /usr/lib/wine is the distro install path; libwine/
   * ntdll cover the in-game loaded modules. Cache the result. */
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) {
    g_wine_detected = 0;
    return false;
  }
  char line[1024];
  int found = 0;
  while (!found && fgets(line, sizeof(line), f)) {
    if (strstr(line, "/libwine") || strstr(line, "/ntdll") || strstr(line, "/usr/lib/wine") ||
        strstr(line, "/usr/lib64/wine")) {
      found = 1;
    }
  }
  fclose(f);
  g_wine_detected = found ? 1 : 0;
  return found == 1;
}

/* Connect to the host-namespace broker, send handshake describing the
 * transport/input sockets the webview should connect to, then wait for
 * an ack. Returns 0 on success and sets g_use_broker so the rest of the
 * overlay skips fork_webview and the compositor uses abstract sockets
 * via IDK_TP_ABSTRACT env. */
int connect_via_broker(void) {
  int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (cfd < 0) {
    IDK_ERR("overlay", "broker: socket() failed: %s\n", strerror(errno));
    return -1;
  }

  char bname[64];
  make_broker_name(bname, sizeof(bname));
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  size_t blen = 1 + strlen(bname + 1);
  memcpy(addr.sun_path, bname, blen);
  if (connect(cfd, (struct sockaddr *)&addr, abstract_addrlen(bname)) < 0) {
    IDK_LOG("overlay", "broker: connect \\0%s failed: %s\n", bname + 1, strerror(errno));
    close(cfd);
    return -1;
  }

  char tp_plain[64]; /* no leading NUL — stored in handshake */
  char in_plain[64];
  snprintf(tp_plain, sizeof(tp_plain), "idk_tp_%d", (int)getpid());
  snprintf(in_plain, sizeof(in_plain), "idk_input_%d", (int)getpid());
  /* Mirror into the local env so the compositor/wayland input pick
   * the matching abstract names up when they call idk_comp_get_path /
   * init_input_socket. */
  setenv("IDK_TP_ABSTRACT", tp_plain, 1);
  setenv("IDK_INPUT_ABSTRACT", in_plain, 1);

  idk_cp_handshake_t hs;
  memset(&hs, 0, sizeof(hs));
  hs.identity = IDK_CP_ID_OVERLAY;
  hs.tp_backend = 0; /* socket backend forced in broker mode */
  const char *no_dmabuf = getenv("IDK_NO_DMABUF");
  hs.no_dmabuf = no_dmabuf && no_dmabuf[0] && no_dmabuf[0] != '0';
  const char *comm = idk_process_name();
  if (comm)
    snprintf(hs.comm, sizeof(hs.comm), "%s", comm);
  snprintf(hs.tp_socket, sizeof(hs.tp_socket), "%s", tp_plain);
  snprintf(hs.input_socket, sizeof(hs.input_socket), "%s", in_plain);

  size_t total = 0;
  while (total < sizeof(hs)) {
    ssize_t n = write(cfd, (const char *)&hs + total, sizeof(hs) - total);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      IDK_ERR("overlay", "broker: handshake write failed: %s\n", strerror(errno));
      close(cfd);
      return -1;
    }
    total += (size_t)n;
  }

  struct pollfd pfd = {.fd = cfd, .events = POLLIN};
  if (poll(&pfd, 1, 5000) <= 0) {
    IDK_ERR("overlay", "broker: ack timeout\n");
    close(cfd);
    return -1;
  }
  uint8_t ack = 0xff;
  ssize_t r = read(cfd, &ack, 1);
  if (r != 1 || ack != 0x00) {
    IDK_ERR("overlay", "broker: ack failed (r=%zd ack=0x%02x)\n", r, ack);
    close(cfd);
    return -1;
  }

  /* Keep cfd open — its EOF is the signal that the broker (and its
   * webview child) is gone. Future monitor thread could read it. */
  fcntl(cfd, F_SETFD, FD_CLOEXEC);
  g_broker_fd = cfd; /* leak intentionally */
  IDK_LOG("overlay", "broker: connected (comm=%s tp=%s input=%s)\n", hs.comm, hs.tp_socket, hs.input_socket);
  return 0;
}
