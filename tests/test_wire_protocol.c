#include "test_runner.h"
#include "public/idk_ipc.h"
#include "core/compositor.h"

TEST(frame_header_size) {
    ASSERT_SIZEOF(idk_frame_header_t, 28);
}

TEST(input_event_size) {
    ASSERT_SIZEOF(idk_input_event_t, 20);
}

TEST(ack_msg_size) {
    ASSERT_SIZEOF(struct idk_ack_msg, 16);
}

TEST(frame_header_offsets) {
    idk_frame_header_t hdr;
    size_t base = (size_t)&hdr;
    ASSERT_EQ((size_t)&hdr.modifier - base, 0);
    ASSERT_EQ((size_t)&hdr.width - base, 8);
    ASSERT_EQ((size_t)&hdr.height - base, 12);
    ASSERT_EQ((size_t)&hdr.stride - base, 16);
    ASSERT_EQ((size_t)&hdr.fourcc - base, 20);
    ASSERT_EQ((size_t)&hdr.flags - base, 24);
    ASSERT_EQ((size_t)&hdr.nfd   - base, 25);
    ASSERT_EQ((size_t)&hdr.buf_id - base, 26);
}

TEST(flag_values) {
    ASSERT_EQ(IDK_FRAME_FLAG_VISIBLE, 0x01);
    ASSERT_EQ(IDK_FRAME_FLAG_DMABUF,  0x02);
}

TEST(frame_is_dmabuf) {
    idk_frame_header_t hdr = {0};
    ASSERT_FALSE(idk_frame_is_dmabuf(&hdr));

    hdr.flags = IDK_FRAME_FLAG_DMABUF;
    ASSERT_TRUE(idk_frame_is_dmabuf(&hdr));

    hdr.flags = IDK_FRAME_FLAG_VISIBLE;
    ASSERT_FALSE(idk_frame_is_dmabuf(&hdr));

    hdr.flags = IDK_FRAME_FLAG_VISIBLE | IDK_FRAME_FLAG_DMABUF;
    ASSERT_TRUE(idk_frame_is_dmabuf(&hdr));
}

TEST(frame_is_visible) {
    idk_frame_header_t hdr = {0};
    ASSERT_FALSE(idk_frame_is_visible(&hdr));

    hdr.flags = IDK_FRAME_FLAG_VISIBLE;
    ASSERT_TRUE(idk_frame_is_visible(&hdr));

    hdr.flags = IDK_FRAME_FLAG_DMABUF;
    ASSERT_FALSE(idk_frame_is_visible(&hdr));

    hdr.flags = IDK_FRAME_FLAG_VISIBLE | IDK_FRAME_FLAG_DMABUF;
    ASSERT_TRUE(idk_frame_is_visible(&hdr));
}

TEST(input_event_flags) {
    ASSERT_EQ(IDK_INPUT_FLAG_PRESS,   0x01);
    ASSERT_EQ(IDK_INPUT_FLAG_CAPTURE, 0x02);
}

TEST(modifier_flags) {
    ASSERT_EQ(IDK_MOD_CTRL,  0x01);
    ASSERT_EQ(IDK_MOD_SHIFT, 0x02);
    ASSERT_EQ(IDK_MOD_ALT,   0x04);
    ASSERT_EQ(IDK_MOD_SUPER, 0x08);
}

TEST(input_type_enum) {
    ASSERT_EQ(IDK_INPUT_KEY,    1);
    ASSERT_EQ(IDK_INPUT_BUTTON, 2);
    ASSERT_EQ(IDK_INPUT_MOTION, 3);
    ASSERT_EQ(IDK_INPUT_AXIS,   4);
    ASSERT_EQ(IDK_INPUT_STATE,  5);
    ASSERT_EQ(IDK_INPUT_REPEAT, 6);
}

TEST(drm_mod_vendor_macro) {
    ASSERT_EQ(IDK_DRM_MOD_VENDOR(0x0300000000000001ULL), 0x03);
    ASSERT_EQ(IDK_DRM_MOD_VENDOR(0x0100000000000001ULL), 0x01);
    ASSERT_EQ(IDK_DRM_MOD_VENDOR(0x0200000000000002ULL), 0x02);
    ASSERT_EQ(IDK_DRM_MOD_VENDOR(0ULL), 0);
}

TEST(invalid_mod_constant) {
    ASSERT_EQ(IDK_DRM_FORMAT_MOD_INVALID, 0x00FFFFFFFFFFFFFFULL);
}

int main(void) {
    RUN(frame_header_size);
    RUN(input_event_size);
    RUN(ack_msg_size);
    RUN(frame_header_offsets);
    RUN(flag_values);
    RUN(frame_is_dmabuf);
    RUN(frame_is_visible);
    RUN(input_event_flags);
    RUN(modifier_flags);
    RUN(input_type_enum);
    RUN(drm_mod_vendor_macro);
    RUN(invalid_mod_constant);
    return 0;
}
