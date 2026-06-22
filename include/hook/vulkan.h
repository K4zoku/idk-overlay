#ifndef IDK_VULKAN_H
#define IDK_VULKAN_H

int idk_vulkan_init(int ipc_fd, const char *socket_path);
void idk_vulkan_shutdown(void);

#endif
