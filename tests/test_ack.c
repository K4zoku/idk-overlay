#include "test_runner.h"
#include "core/compositor.h"

TEST(ack_msg_size) {
    ASSERT_SIZEOF(struct idk_ack_msg, 16);
}

TEST(ack_msg_offsets) {
    struct idk_ack_msg msg;
    size_t base = (size_t)&msg;
    ASSERT_EQ((size_t)&msg.ack  - base, 0);
    ASSERT_EQ((size_t)&msg.w    - base, 4);
    ASSERT_EQ((size_t)&msg.h    - base, 8);
    ASSERT_EQ((size_t)&msg._pad - base, 12);
}

TEST(ack_accept_value) {
    struct idk_ack_msg msg = { .ack = 0, .w = 0, .h = 0 };
    ASSERT_EQ(msg.ack, 0);
}

TEST(ack_reject_value) {
    struct idk_ack_msg msg = { .ack = 1, .w = 0, .h = 0 };
    ASSERT_EQ(msg.ack, 1);
}

TEST(ack_with_resize_info) {
    struct idk_ack_msg msg = { .ack = 0, .w = 1920, .h = 1080 };
    ASSERT_EQ(msg.w, 1920);
    ASSERT_EQ(msg.h, 1080);
}

int main(void) {
    RUN(ack_msg_size);
    RUN(ack_msg_offsets);
    RUN(ack_accept_value);
    RUN(ack_reject_value);
    RUN(ack_with_resize_info);
    return 0;
}
