#ifndef IDK_TP_SOCKET_PROTOCOL_H
#define IDK_TP_SOCKET_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define IDK_SOCKET_V2_MAGIC 0x324B4449u
#define IDK_SOCKET_V2_MAJOR 2u
#define IDK_SOCKET_V2_MAX_PAYLOAD (16u * 1024u * 1024u)

enum idk_socket_v2_type {
  IDK_SOCKET_V2_FRAME = 1,
  IDK_SOCKET_V2_ACK = 2,
  IDK_SOCKET_V2_REQUEST = 3,
  IDK_SOCKET_V2_INPUT = 4,
  IDK_SOCKET_V2_CURSOR = 5,
};

int socket_v2_send(int fd, uint16_t type, const void *payload, size_t payload_bytes, const int *fds, int nfd);
int socket_v2_recv(int fd, uint16_t expected_type, void *payload, size_t payload_capacity, int fds[4], int *nfd);
int socket_v2_peek_type(int fd, uint16_t *type);

#endif
