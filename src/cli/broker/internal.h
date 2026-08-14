#ifndef IDK_BROKER_INTERNAL_H
#define IDK_BROKER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "public/idk_ipc.h"

#define ACK_OK 0x00
#define ACK_ERROR 0x01

struct session_arg {
  int cfd;
};

void make_broker_name(char *buf, size_t bufsz);
int bind_abstract(const char *name);
bool peercred_ok(int fd);
ssize_t recv_full(int fd, void *buf, size_t len);
int validate_handshake(const idk_cp_handshake_t *hs);
pid_t spawn_webview(const idk_cp_handshake_t *hs, int *exec_err_fd);
void reap_webview(pid_t pid);
void *session_thread(void *arg);

#endif /* IDK_BROKER_INTERNAL_H */
