#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/core/transport/socket/protocol.h"
#include "../src/core/transport/socket/internal.h"
#include "core/diagnostics.h"
#include "public/idk_frames.h"
#include "test_runner.h"

#pragma pack(push, 1)
typedef struct test_socket_v2_header {
  uint32_t magic;
  uint16_t major;
  uint16_t minor;
  uint16_t type;
  uint16_t flags;
  uint32_t payload_bytes;
  uint64_t session_generation;
  uint64_t message_id;
} test_socket_v2_header_t;
#pragma pack(pop)

_Static_assert(sizeof(test_socket_v2_header_t) == 32, "test V2 header layout");

TEST(round_trip_frame_and_fd) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
  int payload_fd = open("/dev/null", O_RDONLY);
  ASSERT_TRUE(payload_fd >= 0);
  idk_frame_header_t sent = {.width = 1280, .height = 720, .nfd = 1};
  idk_socket_v2_identity_t sent_identity = {
      .session_generation = socket_v2_new_session_generation(),
      .message_id = 1,
  };
  ASSERT_EQ(socket_v2_send(sockets[0], IDK_SOCKET_V2_FRAME, &sent, sizeof(sent), &sent_identity, &payload_fd, 1), 0);

  idk_frame_header_t received = {0};
  idk_socket_v2_identity_t received_identity = {0};
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  ASSERT_EQ(socket_v2_recv(sockets[1], IDK_SOCKET_V2_FRAME, &received, sizeof(received), &received_identity, fds, &nfd),
            1);
  ASSERT_EQ(received.width, sent.width);
  ASSERT_EQ(received.height, sent.height);
  ASSERT_EQ(received_identity.session_generation, sent_identity.session_generation);
  ASSERT_EQ(received_identity.message_id, sent_identity.message_id);
  ASSERT_EQ(nfd, 1);
  ASSERT_TRUE(fds[0] >= 0);
  close(fds[0]);
  close(payload_fd);
  close(sockets[0]);
  close(sockets[1]);
}

TEST(reject_wrong_type) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
  idk_ack_msg_t ack = {0};
  idk_socket_v2_identity_t identity = {.session_generation = socket_v2_new_session_generation(), .message_id = 1};
  ASSERT_EQ(socket_v2_send(sockets[0], IDK_SOCKET_V2_ACK, &ack, sizeof(ack), &identity, NULL, 0), 0);
  idk_request_msg_t request = {0};
  idk_socket_v2_identity_t received_identity = {0};
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  ASSERT_EQ(socket_v2_recv(sockets[1], IDK_SOCKET_V2_REQUEST, &request, sizeof(request), &received_identity, fds, &nfd),
            -1);
  close(sockets[0]);
  close(sockets[1]);
}

TEST(reject_zero_identity) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
  idk_socket_features_t payload = {0};
  test_socket_v2_header_t header = {
      .magic = IDK_SOCKET_V2_MAGIC,
      .major = IDK_SOCKET_V2_MAJOR,
      .minor = 0,
      .type = IDK_SOCKET_V2_FEATURES,
      .flags = 0,
      .payload_bytes = 0,
      .session_generation = socket_v2_new_session_generation(),
      .message_id = 0,
  };
  ASSERT_EQ(send(sockets[0], &header, sizeof(header), MSG_NOSIGNAL), (ssize_t)sizeof(header));
  idk_socket_v2_identity_t identity = {0};
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  ASSERT_EQ(socket_v2_recv(sockets[1], IDK_SOCKET_V2_FEATURES, &payload, sizeof(payload), &identity, fds, &nfd), -1);
  close(sockets[0]);
  close(sockets[1]);
}

TEST(hello_features_negotiate_without_fds) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
  uint64_t session_generation = socket_v2_new_session_generation();
  idk_socket_v2_identity_t hello_identity = {.session_generation = session_generation, .message_id = 1};
  ASSERT_EQ(socket_v2_send_hello(sockets[0], IDK_SOCKET_FEATURES_DEFAULT, &hello_identity), 0);
  uint32_t server_features = 0;
  uint64_t server_session = 0;
  uint64_t server_next = 0;
  ASSERT_EQ(socket_v2_server_handshake(sockets[1], IDK_SOCKET_FEATURES_DEFAULT, &server_features, &server_session,
                                       &server_next),
            1);
  uint32_t client_features = 0;
  uint64_t client_next = 0;
  ASSERT_EQ(socket_v2_client_handshake(sockets[0], session_generation, &client_features, &client_next), 1);
  ASSERT_TRUE((server_features & IDK_SOCKET_FEATURE_SEQPACKET) != 0);
  ASSERT_EQ(server_features, client_features);
  ASSERT_EQ(server_session, session_generation);
  ASSERT_EQ(server_next, 2);
  ASSERT_EQ(client_next, 2);
  close(sockets[0]);
  close(sockets[1]);
}

TEST(negotiate_capability_subset) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
  uint64_t session_generation = socket_v2_new_session_generation();
  idk_socket_v2_identity_t hello_identity = {.session_generation = session_generation, .message_id = 1};
  ASSERT_EQ(socket_v2_send_hello(sockets[0], IDK_SOCKET_FEATURE_SEQPACKET, &hello_identity), 0);
  uint32_t server_features = 0;
  uint64_t server_session = 0;
  uint64_t server_next = 0;
  ASSERT_EQ(socket_v2_server_handshake(sockets[1], IDK_SOCKET_FEATURES_DEFAULT, &server_features, &server_session,
                                       &server_next),
            1);
  uint32_t client_features = 0;
  uint64_t client_next = 0;
  ASSERT_EQ(socket_v2_client_handshake(sockets[0], session_generation, &client_features, &client_next), 1);
  ASSERT_EQ(server_features, IDK_SOCKET_FEATURE_SEQPACKET);
  ASSERT_EQ(client_features, IDK_SOCKET_FEATURE_SEQPACKET);
  close(sockets[0]);
  close(sockets[1]);
}

TEST(peer_credentials_same_uid) {
  char path[128];
  snprintf(path, sizeof(path), "/tmp/idk-peercred-%d.sock", (int)getpid());
  unlink(path);
  setenv("IDK_TP_SOCKET", "seqpacket", 1);
  setenv("IDK_NO_DMABUF", "1", 1);
  idk_transport_t consumer = {0};
  idk_transport_t producer = {0};
  ASSERT_EQ(idk_tp_init(&consumer, IDK_TP_CONSUMER, path), 0);
  ASSERT_EQ(idk_tp_init(&producer, IDK_TP_PRODUCER, path), 0);
  ASSERT_EQ(idk_tp_accept(&consumer), 1);
  ASSERT_EQ(idk_tp_poll(&consumer), 1);
  ASSERT_TRUE(consumer.ready);
  ASSERT_EQ(idk_tp_poll(&producer), 1);
  ASSERT_TRUE(producer.ready);
  ASSERT_TRUE((tp_socket_features(&consumer) & IDK_SOCKET_FEATURE_DMABUF) == 0);
  ASSERT_TRUE((tp_socket_features(&producer) & IDK_SOCKET_FEATURE_DMABUF) == 0);
  idk_diag_reset();
  idk_request_msg_t request = {.type = IDK_REQUEST_SHUTDOWN};
  ASSERT_EQ(idk_tp_send_request(&producer, &request), 0);
  ASSERT_EQ(idk_diag_get(IDK_DIAG_FRAME_REQUESTED), 0);
  ASSERT_EQ(idk_tp_recv_request(&consumer, &request, 0), 0);
  request.type = IDK_REQUEST_NEXT_FRAME;
  ASSERT_EQ(idk_tp_send_request(&producer, &request), 0);
  ASSERT_EQ(idk_diag_get(IDK_DIAG_FRAME_REQUESTED), 1);
  ASSERT_EQ(idk_tp_recv_request(&consumer, &request, 0), 0);
  idk_socket_v2_identity_t duplicate = {
      .session_generation = tp_socket_session_generation(&producer),
      .message_id = 3,
  };
  ASSERT_EQ(socket_v2_send(producer._client_fd, IDK_SOCKET_V2_REQUEST, &request, sizeof(request), &duplicate, NULL, 0),
            0);
  ASSERT_EQ(idk_tp_recv_request(&consumer, &request, 0), -1);
  idk_tp_destroy(&producer);
  idk_tp_destroy(&consumer);
  unlink(path);
  unsetenv("IDK_TP_SOCKET");
  unsetenv("IDK_NO_DMABUF");
}

int main(void) {
  RUN(round_trip_frame_and_fd);
  RUN(hello_features_negotiate_without_fds);
  RUN(reject_wrong_type);
  RUN(reject_zero_identity);
  RUN(negotiate_capability_subset);
  RUN(peer_credentials_same_uid);
  return 0;
}
