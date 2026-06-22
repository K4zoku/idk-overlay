/*
 * idk_ipc.h — Unix domain socket IPC for passing dmabuf fds + metadata
 *
 * Wire format per frame:
 *   [header: 32 bytes]          [scm_rights: 1 fd]
 *   +------------------+
 *   | width    uint32  |
 *   | height   uint32  |
 *   | stride   uint32  |
 *   | format   uint32  |
 *   | num_planes uint32|
 *   | pid      uint32  |
 *   | reserved uint32  |
 *   | checksum uint32  |
 *   +------------------+
 */
#ifndef IDK_IPC_H
#define IDK_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── IPC constants ─────────────────────────────────────────────────────── */

#define IDK_IPC_SOCKNAME_MAX 108    /* AF_UNIX path max */
#define IDK_IPC_SEND_BUF_SIZE 4096  /* CMSG space for 1 fd */

/* ── IPC socket management ─────────────────────────────────────────────── */

/**
 * Connect to the render process via Unix domain socket.
 * Creates the socket if it doesn't exist yet.
 *
 * @param sockpath  Unix socket path.
 * @param out_fd    Output: the connected socket file descriptor.
 * @return          0 on success, -1 on failure.
 */
int idk_ipc_connect(const char *sockpath, int *out_fd);

/**
 * Close the IPC socket connection.
 *
 * @param fd  Socket fd from idk_ipc_connect().
 */
void idk_ipc_close(int fd);

/* ── Sending frames ────────────────────────────────────────────────────── */

/**
 * Send a frame info + dmabuf fd over the IPC socket.
 * Uses SCM_RIGHTS to pass the fd atomically with the metadata.
 *
 * @param socket_fd   Connected socket fd.
 * @param info        Frame metadata.
 * @param dmabuf_fd   DMABUF fd from the captured frame.
 * @return            0 on success, -1 on failure.
 */
int idk_ipc_send_frame(int socket_fd, const void *info, size_t info_len,
                       int dmabuf_fd);

/* ── Receiving frames (for idk-render process) ─────────────────────────── */

/**
 * Receive a frame info + fd from the IPC socket.
 * Blocks until a frame is available or connection is closed.
 *
 * @param socket_fd  Listening/connected socket fd.
 * @param info       Output: frame metadata struct.
 * @param out_fd     Output: received dmabuf fd (must be closed by caller).
 * @return           0 on success (frame received), -1 on EOF/error.
 */
int idk_ipc_recv_frame(int socket_fd, void *info, size_t info_len,
                       int *out_fd);

#ifdef __cplusplus
}
#endif

#endif /* IDK_IPC_H */
