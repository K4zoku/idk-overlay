#include "test_runner.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/compositor.h"

static socklen_t abstract_addrlen(const char *name) {
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name + 1));
}

static int bind_abstract(const char *name) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    memcpy(addr.sun_path, name, 1 + strlen(name + 1));
    if (bind(fd, (struct sockaddr *)&addr, abstract_addrlen(name)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) { close(fd); return -1; }
    return fd;
}

static int connect_abstract(const char *name) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    memcpy(addr.sun_path, name, 1 + strlen(name + 1));
    if (connect(fd, (struct sockaddr *)&addr, abstract_addrlen(name)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Build an abstract socket name "\0idk_test_<pid>" in buf. */
static void make_test_name(char *buf, size_t bufsz, const char *tag) {
    buf[0] = '\0';
    snprintf(buf + 1, bufsz - 1, "idk_test_%s_%d", tag, (int)getpid());
}

TEST(abstract_listen_accept_connect) {
    char name[64];
    make_test_name(name, sizeof(name), "svc");
    int srv = bind_abstract(name);
    ASSERT_TRUE(srv >= 0);

    int cli = connect_abstract(name);
    ASSERT_TRUE(cli >= 0);

    int acc = accept(srv, NULL, NULL);
    ASSERT_TRUE(acc >= 0);

    close(acc);
    close(cli);
    close(srv);
}

TEST(abstract_scm_rights_single_fd) {
    char name[64];
    make_test_name(name, sizeof(name), "fd");
    int srv = bind_abstract(name);
    ASSERT_TRUE(srv >= 0);

    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        int cli = connect_abstract(name);
        if (cli < 0) _exit(2);
        int pipefd[2];
        if (pipe(pipefd) < 0) _exit(3);
        /* Send the write end; parent writes to it, we read from the
         * read end in this process. */
        struct iovec iov = { .iov_base = "x", .iov_len = 1 };
        char ctrl[CMSG_SPACE(sizeof(int))];
        struct msghdr msg = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = ctrl, .msg_controllen = sizeof(ctrl),
        };
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pipefd[1], sizeof(int));
        if (sendmsg(cli, &msg, 0) < 0) _exit(4);
        close(pipefd[1]);
        char rdbuf[4];
        ssize_t r = read(pipefd[0], rdbuf, sizeof(rdbuf));
        if (r != 2 || memcmp(rdbuf, "hi", 2) != 0) _exit(5);
        close(pipefd[0]);
        close(cli);
        _exit(0);
    }

    int acc = accept(srv, NULL, NULL);
    ASSERT_TRUE(acc >= 0);

    char buf[1];
    char ctrl[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl, .msg_controllen = sizeof(ctrl),
    };
    ssize_t n = recvmsg(acc, &msg, 0);
    ASSERT_TRUE(n == 1);

    int got_fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            memcpy(&got_fd, CMSG_DATA(c), sizeof(int));
            break;
        }
    }
    ASSERT_TRUE(got_fd >= 0);
    ASSERT_TRUE(write(got_fd, "hi", 2) == 2);
    close(got_fd);

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(acc);
    close(srv);
}

TEST(path_based_still_works) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/idk_test_path_%d.sock", (int)getpid());
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", path);
    ASSERT_TRUE(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    ASSERT_TRUE(listen(fd, 1) == 0);

    struct stat st;
    ASSERT_TRUE(stat(path, &st) == 0);
    ASSERT_TRUE(S_ISSOCK(st.st_mode));

    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(cli >= 0);
    ASSERT_TRUE(connect(cli, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    int acc = accept(fd, NULL, NULL);
    ASSERT_TRUE(acc >= 0);

    close(acc); close(cli); close(fd);
    unlink(path);
}

TEST(helper_default_abstract_name) {
    char buf[64];
    idk_comp_get_default_abstract_name(buf, sizeof(buf), 0);
    ASSERT_TRUE(strncmp(buf, "idk_tp_", 7) == 0);
    idk_comp_get_default_abstract_name(buf, sizeof(buf), 1);
    ASSERT_TRUE(strncmp(buf, "idk_input_", 10) == 0);
}

int main(void) {
    RUN(abstract_listen_accept_connect);
    RUN(abstract_scm_rights_single_fd);
    RUN(path_based_still_works);
    RUN(helper_default_abstract_name);
    return 0;
}