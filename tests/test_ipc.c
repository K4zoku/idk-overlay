#include <errno.h>
#include "test_runner.h"
#include "public/idk_ipc.h"

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

int main(void) {
    RUN(send_input_bad_socket);
    RUN(send_input_null_ev);
    RUN(recv_input_bad_socket);
    RUN(recv_input_null_ev);
    return 0;
}
