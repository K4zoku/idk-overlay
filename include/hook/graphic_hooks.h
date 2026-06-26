#ifndef IDK_GRAPHIC_HOOKS_H
#define IDK_GRAPHIC_HOOKS_H

int idk_egl_init(void);
void idk_egl_shutdown(void);

int idk_glx_init(int ipc_fd);
void idk_glx_shutdown(void);

int idk_vulkan_init(int ipc_fd, const char *socket_path);
void idk_vulkan_shutdown(void);

#endif
