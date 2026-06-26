#include "test_runner.h"
#include "core/compositor_common.h"

TEST(nvidia_vendor) {
    ASSERT_EQ(idk_vk_vendor_to_drm(0x10DE), 0x03);
}

TEST(intel_vendor) {
    ASSERT_EQ(idk_vk_vendor_to_drm(0x8086), 0x01);
}

TEST(amd_vendor) {
    ASSERT_EQ(idk_vk_vendor_to_drm(0x1002), 0x02);
}

TEST(unknown_vendor_returns_zero) {
    ASSERT_EQ(idk_vk_vendor_to_drm(0),         0);
    ASSERT_EQ(idk_vk_vendor_to_drm(0x1AE0),    0);  /* Samsung Xclipse */
    ASSERT_EQ(idk_vk_vendor_to_drm(0x13B5),    0);  /* ARM */
    ASSERT_EQ(idk_vk_vendor_to_drm(0x5147),    0);  /* Qualcomm */
    ASSERT_EQ(idk_vk_vendor_to_drm(0xFFFFFFFF), 0);
}

int main(void) {
    RUN(nvidia_vendor);
    RUN(intel_vendor);
    RUN(amd_vendor);
    RUN(unknown_vendor_returns_zero);
    return 0;
}
