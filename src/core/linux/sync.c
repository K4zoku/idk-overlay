
#include "sync.h"

#include <errno.h>
#include <stdlib.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

bool idk_sync_file_enabled(void) {
  const char *value = getenv("IDK_GPU_SYNC");
  return value && value[0] == '1' && value[1] == '\0';
}
int idk_sync_file_wait(int sync_fd) {
  struct pollfd pfd = {.fd = sync_fd, .events = POLLIN | POLLOUT, .revents = 0};
  if (sync_fd < 0) {
    errno = EBADF;
    return -1;
  }
  for (;;) {
    int rc = poll(&pfd, 1, -1);
    if (rc > 0)
      return (pfd.revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) != 0 ? 0 : -1;
    if (rc < 0 && errno == EINTR)
      continue;
    return -1;
  }
}


int idk_dmabuf_export_sync_file(int dmabuf_fd, unsigned int flags) {
  struct dma_buf_export_sync_file export = {.flags = flags, .fd = -1};

  if (dmabuf_fd < 0 || (flags & ~(DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)) != 0 || flags == 0) {
    errno = EINVAL;
    return -1;
  }
  if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export) < 0)
    return -1;
  return export.fd;
}

int idk_dmabuf_import_sync_file(int dmabuf_fd, int sync_fd, unsigned int flags) {
  struct dma_buf_import_sync_file import = {.flags = flags, .fd = sync_fd};

  if (dmabuf_fd < 0 || sync_fd < 0 || (flags & ~(DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)) != 0 || flags == 0) {
    errno = EINVAL;
    return -1;
  }
  return ioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import);
}
