#include "test_runner.h"
#include "public/idk_fs.h"
#include "public/idk_ipc.h"

/* build_frame_hdr is now non-static, declared in public/idk_fs.h */

TEST(build_frame_hdr_copies_all_fields) {
    idk_fs_frame_t frame = {
        .modifier = 0x0300000000000001ULL,
        .width    = 1920,
        .height   = 1080,
        .stride   = 7680,
        .fourcc   = 0x34325241,
        .flags    = 0x03,
    };

    idk_frame_header_t hdr;
    build_frame_hdr(&frame, &hdr);

    ASSERT_EQ(hdr.modifier, 0x0300000000000001ULL);
    ASSERT_EQ(hdr.width,    1920);
    ASSERT_EQ(hdr.height,   1080);
    ASSERT_EQ(hdr.stride,   7680);
    ASSERT_EQ(hdr.fourcc,   0x34325241);
    ASSERT_EQ(hdr.flags,    0x03);
}

TEST(build_frame_hdr_zeroes_padding) {
    idk_fs_frame_t frame = {0};
    idk_frame_header_t hdr;
    memset(&hdr, 0xFF, sizeof(hdr));

    build_frame_hdr(&frame, &hdr);

    ASSERT_EQ(hdr._pad[0], 0);
    ASSERT_EQ(hdr._pad[1], 0);
    ASSERT_EQ(hdr._pad[2], 0);
}

TEST(build_frame_hdr_zero_modifier) {
    idk_fs_frame_t frame = {
        .modifier = 0,
        .width  = 640,
        .height = 480,
    };
    idk_frame_header_t hdr;
    build_frame_hdr(&frame, &hdr);
    ASSERT_EQ(hdr.modifier, 0);
    ASSERT_EQ(hdr.width, 640);
    ASSERT_EQ(hdr.height, 480);
}

TEST(build_frame_hdr_shm_defaults) {
    idk_fs_frame_t frame = {
        .width   = 1280,
        .height  = 720,
        .stride  = 0,
        .fourcc  = 0,
        .flags   = 0x01,
    };
    idk_frame_header_t hdr;
    build_frame_hdr(&frame, &hdr);
    ASSERT_EQ(hdr.stride, 0);
    ASSERT_EQ(hdr.fourcc, 0);
    ASSERT_EQ(hdr.flags,  0x01);
}

int main(void) {
    RUN(build_frame_hdr_copies_all_fields);
    RUN(build_frame_hdr_zeroes_padding);
    RUN(build_frame_hdr_zero_modifier);
    RUN(build_frame_hdr_shm_defaults);
    return 0;
}
