#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "core/compositor.h"
#include "core/log.h"

/* ===== Path helpers ===== */

void idk_comp_get_runtime_dir(char *buf, size_t bufsz) {
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if (xdg && xdg[0]) {
    snprintf(buf, bufsz, "%s", xdg);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
      buf[--n] = '\0';
    return;
  }
  snprintf(buf, bufsz, "/tmp");
}

void idk_comp_get_default_socket_path(char *buf, size_t bufsz, int with_input_suffix) {
  char dir[PATH_MAX];
  idk_comp_get_runtime_dir(dir, sizeof(dir));
  if (with_input_suffix) {
    snprintf(buf, bufsz, "%s/idk-overlay-%d-input", dir, (int)getpid());
  } else {
    snprintf(buf, bufsz, "%s/idk-overlay-%d", dir, (int)getpid());
  }
}

void idk_comp_get_default_abstract_name(char *buf, size_t bufsz, int input) {
  snprintf(buf, bufsz, "idk_%s_%d", input ? "input" : "tp", (int)getpid());
}

void idk_comp_get_path(char *buf, size_t bufsz) {
  const char *abstr = getenv("IDK_TP_ABSTRACT");
  if (abstr && abstr[0]) {
    buf[0] = '\0';
    snprintf(buf + 1, bufsz - 1, "%s", abstr);
    return;
  }
  const char *env = getenv("IDK_SOCKET");
  if (env && env[0]) {
    snprintf(buf, bufsz, "%s", env);
  } else {
    idk_comp_get_default_socket_path(buf, bufsz, 0);
  }
}
