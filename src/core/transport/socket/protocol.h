#ifndef IDK_TP_SOCKET_PROTOCOL_H
#define IDK_TP_SOCKET_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "public/idk_shm_ring.h"
#define IDK_SOCKET_V2_MAGIC 0x324B4449u
#define IDK_SOCKET_V2_MAJOR 2u
#define IDK_SOCKET_V2_MAX_PAYLOAD (16u * 1024u * 1024u)
#define IDK_SOCKET_FEATURE_SEQPACKET (1u << 0)
#define IDK_SOCKET_FEATURE_ROI (1u << 1)
#define IDK_SOCKET_FEATURE_DMABUF (1u << 2)
#define IDK_SOCKET_FEATURE_SYNC_FILE (1u << 3)
#define IDK_SOCKET_FEATURE_SHM_RING (1u << 4)
#define IDK_SOCKET_FEATURES_DEFAULT (IDK_SOCKET_FEATURE_SEQPACKET | IDK_SOCKET_FEATURE_ROI | IDK_SOCKET_FEATURE_DMABUF)

enum idk_socket_v2_type {
  IDK_SOCKET_V2_FRAME = 1,
  IDK_SOCKET_V2_ACK = 2,
  IDK_SOCKET_V2_REQUEST = 3,
  IDK_SOCKET_V2_INPUT = 4,
  IDK_SOCKET_V2_CURSOR = 5,
  IDK_SOCKET_V2_HELLO = 6,
  IDK_SOCKET_V2_FEATURES = 7,
  IDK_SOCKET_V2_RING_CREATE = 8,
  IDK_SOCKET_V2_RING_READY = 9,
  IDK_SOCKET_V2_FRAME_SLOT = 10,
  IDK_SOCKET_V2_SLOT_RELEASE = 11,
  IDK_SOCKET_V2_RING_RETIRE = 12,
};
typedef struct idk_socket_v2_identity {
  uint64_t session_generation;
  uint64_t message_id;
} idk_socket_v2_identity_t;

_Static_assert(sizeof(idk_socket_v2_identity_t) == 16, "idk_socket_v2_identity_t must be 16 bytes");

#pragma pack(push, 1)
typedef struct idk_socket_features {
  uint32_t features;
  uint16_t max_fds;
  uint16_t reserved;
} idk_socket_features_t;
#pragma pack(pop)

_Static_assert(sizeof(idk_socket_features_t) == 8, "idk_socket_features_t must be 8 bytes");

uint64_t socket_v2_new_session_generation(void);
int socket_v2_send_hello(int fd, uint32_t features, const idk_socket_v2_identity_t *identity);
int socket_v2_server_handshake(int fd, uint32_t local_features, uint32_t *features, uint64_t *session_generation,
                               uint64_t *next_message_id);
int socket_v2_client_handshake(int fd, uint64_t session_generation, uint32_t *features, uint64_t *next_message_id);
int socket_v2_send(int fd, uint16_t type, const void *payload, size_t payload_bytes,
                   const idk_socket_v2_identity_t *identity, const int *fds, int nfd);
int socket_v2_recv(int fd, uint16_t expected_type, void *payload, size_t payload_capacity,
                   idk_socket_v2_identity_t *identity, int fds[4], int *nfd);
int socket_v2_peek_type(int fd, uint16_t *type);
int socket_v2_send_ring_create(int fd, const idk_shm_ring_create_t *record, const idk_socket_v2_identity_t *identity,
                               int ring_fd);
int socket_v2_recv_ring_create(int fd, idk_shm_ring_create_t *record, idk_socket_v2_identity_t *identity, int *ring_fd);
int socket_v2_send_ring_ready(int fd, const idk_shm_ring_ready_t *record, const idk_socket_v2_identity_t *identity);
int socket_v2_recv_ring_ready(int fd, idk_shm_ring_ready_t *record, idk_socket_v2_identity_t *identity);
int socket_v2_send_frame_slot(int fd, const idk_shm_ring_frame_slot_t *record,
                              const idk_socket_v2_identity_t *identity);
int socket_v2_recv_frame_slot(int fd, idk_shm_ring_frame_slot_t *record, idk_socket_v2_identity_t *identity);
int socket_v2_send_slot_release(int fd, const idk_shm_ring_slot_release_t *record,
                                const idk_socket_v2_identity_t *identity);
int socket_v2_recv_slot_release(int fd, idk_shm_ring_slot_release_t *record, idk_socket_v2_identity_t *identity);

#endif
