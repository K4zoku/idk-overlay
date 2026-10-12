#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include "../src/core/linux/sync.h"
#include "test_runner.h"

TEST(sync_feature_is_explicitly_opt_in) {
  unsetenv("IDK_GPU_SYNC");
  ASSERT_FALSE(idk_sync_file_enabled());
  setenv("IDK_GPU_SYNC", "1", 1);
  ASSERT_TRUE(idk_sync_file_enabled());
  unsetenv("IDK_GPU_SYNC");
}

TEST(sync_wait_rejects_invalid_fd) {
  errno = 0;
  ASSERT_EQ(idk_sync_file_wait(-1), -1);
  ASSERT_EQ(errno, EBADF);
}

int main(void) {
  RUN(sync_feature_is_explicitly_opt_in);
  RUN(sync_wait_rejects_invalid_fd);
  return 0;
}
