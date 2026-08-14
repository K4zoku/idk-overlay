#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "internal.h"

static socklen_t abstract_addrlen(const char *name) {
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name + 1));
}

void make_broker_name(char *buf, size_t bufsz) {
  buf[0] = '\0';
  snprintf(buf + 1, bufsz - 1, "idk_broker_%d", (int)getuid());
}

int bind_abstract(const char *name) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  memcpy(addr.sun_path, name, 1 + strlen(name + 1));
  if (bind(fd, (struct sockaddr *)&addr, abstract_addrlen(name)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 8) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool peercred_ok(int fd) {
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
    return false;
  return cred.uid == getuid();
}
