#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/core/transport/socket/protocol.h"
#include "core/transport.h"
#include "public/idk_frames.h"
#include "test_runner.h"

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
  idk_transport_t consumer = {0};
  idk_transport_t producer = {0};
  ASSERT_EQ(idk_tp_init(&consumer, IDK_TP_CONSUMER, path), 0);
  ASSERT_EQ(idk_tp_init(&producer, IDK_TP_PRODUCER, path), 0);
  ASSERT_EQ(idk_tp_accept(&consumer), 1);
  ASSERT_EQ(idk_tp_poll(&consumer), 1);
  ASSERT_TRUE(consumer.ready);
  ASSERT_EQ(idk_tp_poll(&producer), 1);
  ASSERT_TRUE(producer.ready);
  idk_tp_destroy(&producer);
  idk_tp_destroy(&consumer);
  unlink(path);
  unsetenv("IDK_TP_SOCKET");
}

int main(void) {
  RUN(round_trip_frame_and_fd);
  RUN(hello_features_negotiate_without_fds);
  RUN(reject_wrong_type);
  RUN(negotiate_capability_subset);
  RUN(peer_credentials_same_uid);
  return 0;
}
