#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/core/transport/socket/protocol.h"
#include "public/idk_frames.h"
#include "test_runner.h"

TEST(round_trip_frame_and_fd) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
  int payload_fd = open("/dev/null", O_RDONLY);
  ASSERT_TRUE(payload_fd >= 0);
  idk_frame_header_t sent = {.width = 1280, .height = 720, .nfd = 1};
  ASSERT_EQ(socket_v2_send(sockets[0], IDK_SOCKET_V2_FRAME, &sent, sizeof(sent), &payload_fd, 1), 0);

  idk_frame_header_t received = {0};
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  ASSERT_EQ(socket_v2_recv(sockets[1], IDK_SOCKET_V2_FRAME, &received, sizeof(received), fds, &nfd), 1);
  ASSERT_EQ(received.width, sent.width);
  ASSERT_EQ(received.height, sent.height);
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
  ASSERT_EQ(socket_v2_send(sockets[0], IDK_SOCKET_V2_ACK, &ack, sizeof(ack), NULL, 0), 0);
  idk_request_msg_t request = {0};
  int fds[4] = {-1, -1, -1, -1};
  int nfd = 0;
  ASSERT_EQ(socket_v2_recv(sockets[1], IDK_SOCKET_V2_REQUEST, &request, sizeof(request), fds, &nfd), -1);
  close(sockets[0]);
  close(sockets[1]);
}

int main(void) {
  RUN(round_trip_frame_and_fd);
  RUN(reject_wrong_type);
  return 0;
}
