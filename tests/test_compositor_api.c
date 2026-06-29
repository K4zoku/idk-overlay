#include "test_runner.h"
#include "core/compositor.h"

TEST(has_frame_returns_false_initially) {
    int r = idk_compositor_has_frame();
    ASSERT_EQ(r, 0);
}

TEST(notify_resize_wrapper) {
    idk_compositor_notify_resize(1920, 1080);
    ASSERT_EQ(g_comp.game_w, 1920);
    ASSERT_EQ(g_comp.game_h, 1080);
    ASSERT_TRUE(g_comp.size_pending);
}

TEST(shutdown_no_crash) {
    /* Safe to call even when not inited */
    idk_compositor_shutdown();
    ASSERT_FALSE(g_comp.inited);
}

TEST(init_twice_noop) {
    int r1 = idk_compositor_init();
    if (r1 == 0) {
        int r2 = idk_compositor_init();
        ASSERT_EQ(r2, 0);
        ASSERT_TRUE(g_comp.inited);
        idk_compositor_shutdown();
    }
    /* If r1 != 0 (no transport available), still pass — init returns -1 */
}

int main(void) {
    RUN(has_frame_returns_false_initially);
    RUN(notify_resize_wrapper);
    RUN(shutdown_no_crash);
    RUN(init_twice_noop);
    return 0;
}
