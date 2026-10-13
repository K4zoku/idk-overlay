#include <cstring>

#include "../src/cli/webview_cef/shm_ring.h"
#include "test_runner.h"

TEST(ring_allocates_and_releases_slots) {
    ShmRing ring;
    ASSERT_TRUE(ring.Init(7, 2, 4096, 8192));
    ASSERT_TRUE(ring.fd() >= 0);
    ASSERT_EQ(ring.mapped_bytes(), 8192);
    const char payload[] = "ring";
    int first = ring.Acquire(1, sizeof(payload));
    int second = ring.Acquire(2, sizeof(payload));
    ASSERT_TRUE(first >= 0);
    ASSERT_TRUE(second >= 0);
    ASSERT_EQ(ring.Acquire(3, sizeof(payload)), -1);
    ASSERT_TRUE(ring.Write(first, 0, payload, sizeof(payload)));
    ASSERT_TRUE(!ring.Release(first, 8, 1));
    ASSERT_TRUE(ring.Release(first, 7, 1));
    ASSERT_EQ(ring.Acquire(3, sizeof(payload)), first);
    ASSERT_TRUE(ring.Release(first, 7, 3));
    ASSERT_TRUE(ring.Release(second, 7, 2));
}

TEST(ring_rejects_invalid_bounds) {
    ShmRing ring;
    ASSERT_TRUE(ring.Init(9, 2, 64, 128));
    const char payload[] = "x";
    int slot = ring.Acquire(1, sizeof(payload));
    ASSERT_TRUE(slot >= 0);
    ASSERT_TRUE(!ring.Write(slot, 64, payload, 1));
    ASSERT_TRUE(!ring.Write(slot, 63, payload, 2));
    ASSERT_TRUE(ring.Release(slot, 9, 1));
}

int main() {
    RUN(ring_allocates_and_releases_slots);
    RUN(ring_rejects_invalid_bounds);
    return 0;
}
