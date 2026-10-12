#ifndef IDK_LINUX_FD_H
#define IDK_LINUX_FD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int idk_memfd_copy(const void *src, size_t size, const char *label);
int idk_fd_dup_cloexec(int fd);
void idk_fd_close_many(int *fds, size_t count);
int idk_fd_set_size_seals(int fd);

#ifdef __cplusplus
}
#endif

#endif
