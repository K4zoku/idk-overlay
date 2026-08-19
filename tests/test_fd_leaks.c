#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/core/transport/shm/internal.h"
#include "../src/core/transport/socket/internal.h"
#include "core/compositor.h"
#include "test_runner.h"

static int count_open_fds(void) {
  DIR *dir = opendir("/proc/self/fd");
  ASSERT_NE(dir, NULL);
  int count = 0;
  while (readdir(dir))
    count++;
  closedir(dir);
  return count;
}

static void close_fds(int fds[4]) {
  for (int i = 0; i < 4; i++) {
    if (fds[i] >= 0)
      close(fds[i]);
    fds[i] = -1;
  }
}

static ssize_t send_fds(int sock, const void *buf, size_t len, const int *fds, int nfd) {
  char control[CMSG_SPACE(sizeof(int) * 5)] = {0};
  struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = control,
      .msg_controllen = sizeof(control),
  };
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
  memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfd);
  msg.msg_controllen = cmsg->cmsg_len;
  return sendmsg(sock, &msg, MSG_NOSIGNAL);
}

TEST(scm_rights_plateau) {
  int baseline = count_open_fds();
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

  idk_frame_header_t frame = {.width = 64, .height = 64};
  for (int iteration = 0; iteration < 200; iteration++) {
    int nfd = (iteration % 4) + 1;
    int sent[4] = {-1, -1, -1, -1};
    for (int i = 0; i < nfd; i++) {
      sent[i] = open("/dev/null", O_RDONLY);
      ASSERT_TRUE(sent[i] >= 0);
    }
    ASSERT_EQ(scm_send_fds(sockets[0], &frame, sizeof(frame), sent, nfd), (ssize_t)sizeof(frame));
    for (int i = 0; i < nfd; i++)
      close(sent[i]);

    int received[4] = {-1, -1, -1, -1};
    int received_nfd = 0;
    ASSERT_EQ(scm_recv_fds(sockets[1], &frame, sizeof(frame), received, &received_nfd), 1);
    ASSERT_EQ(received_nfd, nfd);
    close_fds(received);
  }

  close(sockets[0]);
  close(sockets[1]);
  ASSERT_TRUE(count_open_fds() <= baseline + 2);
}

TEST(scm_malformed_messages_close_received_fds) {
  int baseline = count_open_fds();
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  idk_frame_header_t frame = {.width = 64, .height = 64};

  for (int iteration = 0; iteration < 100; iteration++) {
    int sent = open("/dev/null", O_RDONLY);
    ASSERT_TRUE(sent >= 0);
    ASSERT_EQ(send_fds(sockets[0], &frame, sizeof(frame) - 1, &sent, 1), (ssize_t)sizeof(frame) - 1);
    close(sent);

    int received[4] = {-1, -1, -1, -1};
    int received_nfd = 0;
    ASSERT_EQ(scm_recv_fds(sockets[1], &frame, sizeof(frame), received, &received_nfd), -1);
    ASSERT_EQ(received_nfd, 0);
  }

  for (int iteration = 0; iteration < 100; iteration++) {
    int sent[5];
    for (int i = 0; i < 5; i++) {
      sent[i] = open("/dev/null", O_RDONLY);
      ASSERT_TRUE(sent[i] >= 0);
    }
    ASSERT_EQ(send_fds(sockets[0], &frame, sizeof(frame), sent, 5), (ssize_t)sizeof(frame));
    for (int i = 0; i < 5; i++)
      close(sent[i]);

    int received[4] = {-1, -1, -1, -1};
    int received_nfd = 0;
    ASSERT_EQ(scm_recv_fds(sockets[1], &frame, sizeof(frame), received, &received_nfd), -1);
    ASSERT_EQ(received_nfd, 0);
  }

  close(sockets[0]);
  close(sockets[1]);
  ASSERT_TRUE(count_open_fds() <= baseline + 2);
}

TEST(frame_fd_cleanup_is_idempotent) {
  int baseline = count_open_fds();
  int fds[4];
  for (int i = 0; i < 4; i++) {
    fds[i] = open("/dev/null", O_RDONLY);
    ASSERT_TRUE(fds[i] >= 0);
  }
  int nfd = 4;
  idk_compositor_close_frame_fds(fds, &nfd);
  ASSERT_EQ(nfd, 0);
  for (int i = 0; i < 4; i++)
    ASSERT_EQ(fds[i], -1);
  idk_compositor_close_frame_fds(fds, &nfd);
  ASSERT_TRUE(count_open_fds() <= baseline + 2);
}

TEST(shm_partial_steal_closes_previous_fds) {
  int baseline = count_open_fds();
  idk_transport_t tp = {._client_fd = -1};
  unsigned char state[SHM_SIZE] = {0};
  *shm_i32(state, SHM_O_PROD_PID) = (int32_t)getpid();
  int fds[4] = {open("/dev/null", O_RDONLY), -1, -1, -1};
  ASSERT_TRUE(fds[0] >= 0);
  ASSERT_EQ(shm_steal_fd(&tp, state, -1, 1, fds), -1);
  ASSERT_EQ(fds[0], -1);
  if (tp._client_fd >= 0)
    close(tp._client_fd);
  ASSERT_TRUE(count_open_fds() <= baseline + 2);
}

TEST(compositor_replacement_and_shutdown_plateau) {
  setenv("IDK_TP_BACKEND", "socket", 1);
  char path[128];
  snprintf(path, sizeof(path), "/tmp/idk-fd-test-%d", (int)getpid());
  unlink(path);
  setenv("IDK_SOCKET", path, 1);

  int baseline = count_open_fds();
  idk_compositor_shutdown();
  ASSERT_EQ(idk_compositor_init(), 0);

  idk_transport_t producer = {0};
  ASSERT_EQ(idk_tp_init(&producer, IDK_TP_PRODUCER, path), 0);
  for (int iteration = 0; iteration < 100; iteration++) {
    int sent[4] = {-1, -1, -1, -1};
    for (int i = 0; i < 4; i++) {
      sent[i] = open("/dev/null", O_RDONLY);
      ASSERT_TRUE(sent[i] >= 0);
    }
    idk_frame_header_t frame = {
        .width = 64, .height = 64, .flags = IDK_FRAME_FLAG_VISIBLE | IDK_FRAME_FLAG_DMABUF, .nfd = 4};
    ASSERT_EQ(idk_tp_send(&producer, &frame, sent, 4), 0);
    for (int i = 0; i < 4; i++)
      close(sent[i]);
    ASSERT_EQ(idk_compositor_recv_frame(true), 1);
  }

  idk_tp_destroy(&producer);
  idk_compositor_shutdown();
  unlink(path);
  ASSERT_TRUE(count_open_fds() <= baseline + 2);
}

int main(void) {
  RUN(scm_rights_plateau);
  RUN(scm_malformed_messages_close_received_fds);
  RUN(frame_fd_cleanup_is_idempotent);
  RUN(shm_partial_steal_closes_previous_fds);
  RUN(compositor_replacement_and_shutdown_plateau);
  return 0;
}
