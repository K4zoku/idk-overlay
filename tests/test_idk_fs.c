#include <errno.h>
#include <string.h>
#include "test_runner.h"
#include "public/idk_fs.h"
#include "public/idk_ipc.h"

TEST(init2_null_path) {
    errno = 0;
    ASSERT_EQ(idk_fs_init2(NULL, 0, 0), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(init2_path_too_long) {
    char long_path[200];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    errno = 0;
    ASSERT_EQ(idk_fs_init2(long_path, 0, 0), -1);
    ASSERT_EQ(errno, ERANGE);
}

TEST(send_frame_null) {
    errno = 0;
    ASSERT_EQ(idk_fs_send_frame(0, NULL), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(send_pixels_null_pixels) {
    idk_fs_frame_t frame = { .width = 100, .height = 100 };
    errno = 0;
    ASSERT_EQ(idk_fs_send_pixels(NULL, &frame), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(send_pixels_null_frame) {
    char buf[4] = {0};
    errno = 0;
    ASSERT_EQ(idk_fs_send_pixels(buf, NULL), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(send_dma_buf_null_fds) {
    idk_fs_frame_t frame = { .nfd = 1 };
    errno = 0;
    ASSERT_EQ(idk_fs_send_dma_buf(NULL, &frame), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(send_dma_buf_null_frame) {
    int fd = 0;
    errno = 0;
    ASSERT_EQ(idk_fs_send_dma_buf(&fd, NULL), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(send_dma_buf_nfd_zero) {
    idk_fs_frame_t frame = { .nfd = 0 };
    int fd = 0;
    errno = 0;
    ASSERT_EQ(idk_fs_send_dma_buf(&fd, &frame), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(send_dma_buf_nfd_too_high) {
    idk_fs_frame_t frame = { .nfd = 5 };
    int fd = 0;
    errno = 0;
    ASSERT_EQ(idk_fs_send_dma_buf(&fd, &frame), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(wait_ack_not_connected) {
    idk_fs_shutdown();
    errno = 0;
    ASSERT_EQ(idk_fs_wait_ack(NULL, 0), -1);
    ASSERT_EQ(errno, ENOTCONN);
}

TEST(get_fd_default) {
    idk_fs_shutdown();
    ASSERT_EQ(idk_fs_get_fd(), -1);
}

TEST(shutdown_idempotent) {
    idk_fs_shutdown();
    idk_fs_shutdown();
    idk_fs_shutdown();
}

int main(void) {
    RUN(init2_null_path);
    RUN(init2_path_too_long);
    RUN(send_frame_null);
    RUN(send_pixels_null_pixels);
    RUN(send_pixels_null_frame);
    RUN(send_dma_buf_null_fds);
    RUN(send_dma_buf_null_frame);
    RUN(send_dma_buf_nfd_zero);
    RUN(send_dma_buf_nfd_too_high);
    RUN(wait_ack_not_connected);
    RUN(get_fd_default);
    RUN(shutdown_idempotent);
    return 0;
}
