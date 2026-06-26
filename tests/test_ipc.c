#include <errno.h>
#include "test_runner.h"
#include "public/idk_ipc.h"

TEST(send_frame_bad_socket) {
    errno = 0;
    idk_frame_header_t hdr = {0};
    ASSERT_EQ(idk_ipc_send_frame(-1, &hdr, 0), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(send_frame_null_hdr) {
    errno = 0;
    ASSERT_EQ(idk_ipc_send_frame(0, NULL, 0), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(recv_frame_bad_socket) {
    errno = 0;
    idk_frame_header_t hdr;
    int fd;
    ASSERT_EQ(idk_ipc_recv_frame(-1, &hdr, &fd), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(recv_frame_null_hdr) {
    errno = 0;
    int fd;
    ASSERT_EQ(idk_ipc_recv_frame(0, NULL, &fd), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(recv_frame_null_out_fd) {
    errno = 0;
    idk_frame_header_t hdr;
    ASSERT_EQ(idk_ipc_recv_frame(0, &hdr, NULL), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(send_input_bad_socket) {
    errno = 0;
    idk_input_event_t ev = {0};
    ASSERT_EQ(idk_ipc_send_input(-1, &ev), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(send_input_null_ev) {
    errno = 0;
    ASSERT_EQ(idk_ipc_send_input(0, NULL), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(recv_input_bad_socket) {
    errno = 0;
    idk_input_event_t ev;
    ASSERT_EQ(idk_ipc_recv_input(-1, &ev, 0), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(recv_input_null_ev) {
    errno = 0;
    ASSERT_EQ(idk_ipc_recv_input(0, NULL, 0), -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(close_negative_fd) {
    idk_ipc_close(-1);
    idk_ipc_close(-999);
}

int main(void) {
    RUN(send_frame_bad_socket);
    RUN(send_frame_null_hdr);
    RUN(recv_frame_bad_socket);
    RUN(recv_frame_null_hdr);
    RUN(recv_frame_null_out_fd);
    RUN(send_input_bad_socket);
    RUN(send_input_null_ev);
    RUN(recv_input_bad_socket);
    RUN(recv_input_null_ev);
    RUN(close_negative_fd);
    return 0;
}
