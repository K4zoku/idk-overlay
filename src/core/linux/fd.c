
#include "fd.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int create_memfd(const char *label) {
  return (int)syscall(SYS_memfd_create, label ? label : "idk-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
}

static int create_shm_fallback(const char *label) {
  char name[96];
  static unsigned int sequence;
  int fd;

  snprintf(name, sizeof(name), "/idk-%s-%ld-%u", label ? label : "frame", (long)getpid(), ++sequence);
  fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0)
    (void)shm_unlink(name);
  return fd;
}

int idk_fd_set_size_seals(int fd) {
  if (fd < 0)
    return -1;
  return fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
}

int idk_memfd_copy(const void *src, size_t size, const char *label) {
  int fd;
  void *mapping;

  if (!src || size == 0 || size > (size_t)INT64_MAX) {
    errno = EINVAL;
    return -1;
  }

  fd = create_memfd(label);
  if (fd < 0 && (errno == ENOSYS || errno == EINVAL || errno == EPERM))
    fd = create_shm_fallback(label);
  if (fd < 0)
    return -1;

  if (ftruncate(fd, (off_t)size) < 0) {
    close(fd);
    return -1;
  }

  mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    close(fd);
    return -1;
  }
  memcpy(mapping, src, size);
  if (munmap(mapping, size) < 0) {
    close(fd);
    return -1;
  }

  if (idk_fd_set_size_seals(fd) < 0 && errno != EINVAL) {
    close(fd);
    return -1;
  }
  return fd;
}

int idk_fd_dup_cloexec(int fd) {
  if (fd < 0) {
    errno = EBADF;
    return -1;
  }
#ifdef F_DUPFD_CLOEXEC
  return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
  int copy = dup(fd);
  if (copy >= 0) {
    int flags = fcntl(copy, F_GETFD, 0);
    if (flags >= 0)
      (void)fcntl(copy, F_SETFD, flags | FD_CLOEXEC);
  }
  return copy;
#endif
}

void idk_fd_close_many(int *fds, size_t count) {
  if (!fds)
    return;
  for (size_t i = 0; i < count; i++) {
    if (fds[i] >= 0) {
      close(fds[i]);
      fds[i] = -1;
    }
  }
}
