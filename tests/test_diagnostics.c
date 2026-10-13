#include "core/diagnostics.h"
#include "test_runner.h"

TEST(counters_reset_and_increment) {
    idk_diag_reset();
    ASSERT_EQ(idk_diag_get(IDK_DIAG_FRAME_SENT_FULL), 0);
    idk_diag_inc(IDK_DIAG_FRAME_SENT_FULL);
    idk_diag_inc(IDK_DIAG_FRAME_SENT_FULL);
    ASSERT_EQ(idk_diag_get(IDK_DIAG_FRAME_SENT_FULL), 2);
    idk_diag_inc(IDK_DIAG_FRAME_SENT_ROI);
    ASSERT_EQ(idk_diag_get(IDK_DIAG_FRAME_SENT_ROI), 1);
    idk_diag_reset();
    ASSERT_EQ(idk_diag_get(IDK_DIAG_FRAME_SENT_FULL), 0);
    ASSERT_EQ(idk_diag_get(IDK_DIAG_FRAME_SENT_ROI), 0);
}

TEST(invalid_counter_is_ignored) {
    idk_diag_reset();
    idk_diag_inc((enum idk_diag_counter)-1);
    idk_diag_inc(IDK_DIAG_COUNTER_COUNT);
    ASSERT_EQ(idk_diag_get((enum idk_diag_counter)-1), 0);
    ASSERT_EQ(idk_diag_get(IDK_DIAG_COUNTER_COUNT), 0);
}

int main(void) {
    RUN(counters_reset_and_increment);
    RUN(invalid_counter_is_ignored);
    return 0;
}
