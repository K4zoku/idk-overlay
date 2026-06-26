#include "test_runner.h"
#include "core/compositor_common.h"

TEST(notify_resize_detects_change) {
    int game_w = 500, game_h = 500;
    bool size_pending = false;
    struct timespec ts = {0};
    bool changed = idk_comp_notify_resize(&game_w, &game_h, &size_pending,
                                          &ts, 800, 600, "test");
    ASSERT_TRUE(changed);
    ASSERT_EQ(game_w, 800);
    ASSERT_EQ(game_h, 600);
    ASSERT_TRUE(size_pending);
    ASSERT_NE(ts.tv_sec, 0);
}

TEST(notify_resize_nochange) {
    int game_w = 800, game_h = 600;
    bool size_pending = false;
    struct timespec ts = {0};
    bool changed = idk_comp_notify_resize(&game_w, &game_h, &size_pending,
                                          &ts, 800, 600, "test");
    ASSERT_FALSE(changed);
    ASSERT_EQ(game_w, 800);
    ASSERT_EQ(game_h, 600);
    ASSERT_FALSE(size_pending);
}

TEST(notify_resize_rejects_invalid) {
    int game_w = 800, game_h = 600;
    bool size_pending = false;
    struct timespec ts = {0};

    ASSERT_FALSE(idk_comp_notify_resize(&game_w, &game_h, &size_pending,
                                        &ts, 0, 600, "test"));
    ASSERT_FALSE(idk_comp_notify_resize(&game_w, &game_h, &size_pending,
                                        &ts, 800, 0, "test"));
    ASSERT_FALSE(idk_comp_notify_resize(&game_w, &game_h, &size_pending,
                                        &ts, -1, 600, "test"));
    ASSERT_FALSE(idk_comp_notify_resize(&game_w, &game_h, &size_pending,
                                        &ts, 800, -1, "test"));
}

TEST(resize_stable_after_debounce) {
    struct timespec old;
    clock_gettime(CLOCK_MONOTONIC, &old);
    /* Simulate that 100ms passed */
    old.tv_nsec -= 100000000; /* subtract 100ms, then add offset */
    if (old.tv_nsec < 0) {
        old.tv_sec -= 1;
        old.tv_nsec += 1000000000L;
    }
    /* Wait a bit so "now" is definitely > old + 50ms */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ASSERT_TRUE(idk_comp_resize_stable(&old, 50));
}

TEST(resize_not_stable_before_debounce) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* With ts == now, debounce of 50ms should not be satisfied */
    ASSERT_FALSE(idk_comp_resize_stable(&ts, 50));
}

TEST(resize_stable_zero_timestamp) {
    struct timespec ts = {0, 0};
    ASSERT_TRUE(idk_comp_resize_stable(&ts, 50));
}

int main(void) {
    RUN(notify_resize_detects_change);
    RUN(notify_resize_nochange);
    RUN(notify_resize_rejects_invalid);
    RUN(resize_stable_after_debounce);
    RUN(resize_not_stable_before_debounce);
    RUN(resize_stable_zero_timestamp);
    return 0;
}
