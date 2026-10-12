#ifndef IDK_LINUX_SYNC_H
#define IDK_LINUX_SYNC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool idk_sync_file_enabled(void);
int idk_dmabuf_export_sync_file(int dmabuf_fd, unsigned int flags);
int idk_sync_file_wait(int sync_fd);
int idk_dmabuf_import_sync_file(int dmabuf_fd, int sync_fd, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif
