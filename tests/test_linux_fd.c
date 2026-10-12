#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "../src/core/linux/fd.h"
#include "test_runner.h"

TEST(memfd_snapshot_is_sealed) {
  const char payload[] = "idk-frame";
  int fd = idk_memfd_copy(payload, sizeof(payload), "test-frame");
  ASSERT_TRUE(fd >= 0);

  char received[sizeof(payload)] = {0};
  ASSERT_EQ(pread(fd, received, sizeof(received), 0), (ssize_t)sizeof(received));
  ASSERT_EQ(memcmp(received, payload, sizeof(payload)), 0);
  ASSERT_TRUE((fcntl(fd, F_GET_SEALS) & (F_SEAL_SHRINK | F_SEAL_GROW)) == (F_SEAL_SHRINK | F_SEAL_GROW));
  errno = 0;
  ASSERT_EQ(ftruncate(fd, (off_t)sizeof(payload) + 1), -1);
  ASSERT_TRUE(errno == EPERM || errno == EINVAL);
  close(fd);
}

TEST(close_many_accepts_fd_zero) {
  int original = dup(STDIN_FILENO);
  int replacement = open("/dev/null", O_RDONLY);
  ASSERT_TRUE(original >= 0 && replacement >= 0);
  ASSERT_EQ(dup2(replacement, STDIN_FILENO), STDIN_FILENO);
  int fds[1] = {STDIN_FILENO};
  idk_fd_close_many(fds, 1);
  ASSERT_EQ(fds[0], -1);
  ASSERT_EQ(dup2(original, STDIN_FILENO), STDIN_FILENO);
  close(original);
  close(replacement);
}

int main(void) {
  RUN(memfd_snapshot_is_sealed);
  RUN(close_many_accepts_fd_zero);
  return 0;
}
