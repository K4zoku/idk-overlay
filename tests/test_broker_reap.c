#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/cli/broker/internal.h"
#include "test_runner.h"

static void assert_reaped(pid_t pid) {
    int status = 0;
    errno = 0;
    ASSERT_EQ(waitpid(pid, &status, WNOHANG), -1);
    ASSERT_EQ(errno, ECHILD);
}

TEST(reap_fallback_short_lived_child) {
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0)
        _exit(0);
    reap_webview(pid, -1);
    assert_reaped(pid);
}

TEST(reap_pidfd_or_fallback) {
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        alarm(2);
        pause();
        _exit(124);
    }
    errno = 0;
    int pidfd = broker_pidfd_open(pid);
    if (pidfd < 0 && (errno == ENOSYS || errno == EINVAL || errno == EPERM)) {
        reap_webview(pid, -1);
        assert_reaped(pid);
        return;
    }
    ASSERT_TRUE(pidfd >= 0);
    reap_webview(pid, pidfd);
    assert_reaped(pid);
}

int main(void) {
    RUN(reap_fallback_short_lived_child);
    RUN(reap_pidfd_or_fallback);
    return 0;
}
