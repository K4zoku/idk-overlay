#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "test_runner.h"
#include "core/compositor.h"

TEST(init_zeros) {
    idk_shm_cache_t c;
    memset(&c, 0xFF, sizeof(c));
    idk_shm_cache_init(&c);
    ASSERT_EQ(c.map, NULL);
    ASSERT_EQ(c.size, 0);
    ASSERT_EQ(c.ino, 0);
    ASSERT_EQ(c.dev, 0);
}

TEST(map_invalid_fd_returns_null) {
    idk_shm_cache_t c;
    idk_shm_cache_init(&c);
    ASSERT_EQ(idk_shm_cache_map(&c, -1), NULL);
    ASSERT_EQ(idk_shm_cache_map(&c, 9999), NULL);
}

TEST(map_valid_memfd_returns_pointer) {
    int fd = memfd_create("test-shm", 0);
    ASSERT_NE(fd, -1);
    ASSERT_EQ(ftruncate(fd, 4096), 0);

    idk_shm_cache_t c;
    idk_shm_cache_init(&c);
    void *p = idk_shm_cache_map(&c, fd);
    ASSERT_NE(p, NULL);
    ASSERT_EQ(c.size, 4096);
    close(fd);
}

TEST(map_caches_same_fd) {
    int fd = memfd_create("test-shm", 0);
    ASSERT_NE(fd, -1);
    ASSERT_EQ(ftruncate(fd, 4096), 0);

    idk_shm_cache_t c;
    idk_shm_cache_init(&c);
    void *p1 = idk_shm_cache_map(&c, fd);
    ASSERT_NE(p1, NULL);
    void *p2 = idk_shm_cache_map(&c, fd);
    ASSERT_EQ(p1, p2);
    close(fd);
}

TEST(map_different_fd_remaps) {
    int fd1 = memfd_create("test-shm-1", 0);
    ASSERT_NE(fd1, -1);
    ASSERT_EQ(ftruncate(fd1, 4096), 0);

    int fd2 = memfd_create("test-shm-2", 0);
    ASSERT_NE(fd2, -1);
    ASSERT_EQ(ftruncate(fd2, 8192), 0);

    idk_shm_cache_t c;
    idk_shm_cache_init(&c);
    void *p1 = idk_shm_cache_map(&c, fd1);
    ASSERT_NE(p1, NULL);
    ASSERT_EQ(c.size, 4096);

    void *p2 = idk_shm_cache_map(&c, fd2);
    ASSERT_NE(p2, NULL);
    ASSERT_NE(p1, p2);
    ASSERT_EQ(c.size, 8192);
    close(fd1);
    close(fd2);
}

TEST(cleanup_zeros) {
    int fd = memfd_create("test-shm", 0);
    ASSERT_NE(fd, -1);
    ASSERT_EQ(ftruncate(fd, 4096), 0);

    idk_shm_cache_t c;
    idk_shm_cache_init(&c);
    ASSERT_NE(idk_shm_cache_map(&c, fd), NULL);
    idk_shm_cache_cleanup(&c);
    ASSERT_EQ(c.map, NULL);
    ASSERT_EQ(c.size, 0);
    ASSERT_EQ(c.ino, 0);
    ASSERT_EQ(c.dev, 0);
    close(fd);
}

int main(void) {
    RUN(init_zeros);
    RUN(map_invalid_fd_returns_null);
    RUN(map_valid_memfd_returns_pointer);
    RUN(map_caches_same_fd);
    RUN(map_different_fd_remaps);
    RUN(cleanup_zeros);
    return 0;
}
