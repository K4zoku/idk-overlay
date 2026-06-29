#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_runner.h"
#include "core/compositor.h"

TEST(get_path_default) {
    char buf[512];
    idk_comp_get_path(buf, sizeof(buf));
    /* No env set → filesystem path starting with / or /tmp */
    ASSERT_TRUE(buf[0] == '/');
}

TEST(get_path_socket_env) {
    setenv("IDK_SOCKET", "/tmp/idk-test-socket", 1);
    char buf[512];
    idk_comp_get_path(buf, sizeof(buf));
    ASSERT_STREQ(buf, "/tmp/idk-test-socket");
    unsetenv("IDK_SOCKET");
}

TEST(get_path_abstract_env) {
    setenv("IDK_TP_ABSTRACT", "idk_tp_test", 1);
    char buf[512] = {0};
    idk_comp_get_path(buf, sizeof(buf));
    ASSERT_EQ(buf[0], '\0');
    ASSERT_STREQ(buf + 1, "idk_tp_test");
    unsetenv("IDK_TP_ABSTRACT");
}

TEST(get_path_abstract_overrides_socket) {
    setenv("IDK_SOCKET", "/tmp/should-not-appear", 1);
    setenv("IDK_TP_ABSTRACT", "idk_override", 1);
    char buf[512] = {0};
    idk_comp_get_path(buf, sizeof(buf));
    ASSERT_EQ(buf[0], '\0');
    ASSERT_STREQ(buf + 1, "idk_override");
    unsetenv("IDK_SOCKET");
    unsetenv("IDK_TP_ABSTRACT");
}

TEST(get_runtime_dir) {
    char buf[256];
    idk_comp_get_runtime_dir(buf, sizeof(buf));
    ASSERT_TRUE(buf[0] == '/');
}

int main(void) {
    RUN(get_path_default);
    RUN(get_path_socket_env);
    RUN(get_path_abstract_env);
    RUN(get_path_abstract_overrides_socket);
    RUN(get_runtime_dir);
    return 0;
}
