#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "internal.h"

/* Webview binary name. Priority: IDK_WEBVIEW_BIN (explicit path) >
 * IDK_WEBVIEW (name, e.g. "idk-webview-cef") > default. */
static const char *webview_bin(void) {
  const char *bin = getenv("IDK_WEBVIEW_BIN");
  if (bin && bin[0])
    return bin;
  bin = getenv("IDK_WEBVIEW");
  if (bin && bin[0])
    return bin;
  return "idk-webview";
}

/* fork+exec the webview; returns child pid, or -1 on failure.
 * exec_err_pipe[1] is set CLOEXEC and the parent reads its end to
 * detect exec failure (write on failure → 0 bytes on success). */
pid_t spawn_webview(const idk_cp_handshake_t *hs, int *exec_err_fd) {
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) < 0)
    return -1;
  *exec_err_fd = pipefd[0];

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    close(pipefd[0]);
    extern char **environ;
    setenv("IDK_TP_BACKEND", "socket", 1);
    char buf[80];
    snprintf(buf, sizeof(buf), "%s", hs->tp_socket);
    setenv("IDK_TP_ABSTRACT", buf, 1);
    snprintf(buf, sizeof(buf), "%s", hs->input_socket);
    setenv("IDK_INPUT_ABSTRACT", buf, 1);
    setenv("IDK_MATCH", hs->comm, 1);
    const char *bin = webview_bin();
    char *argv[4] = {(char *)bin, NULL, NULL, NULL};
    if (hs->no_dmabuf)
      argv[1] = (char *)"--no-dmabuf";
    execvp(bin, argv);
    int err = errno;
    (void)!write(pipefd[1], &err, sizeof(err));
    _exit(127);
  }
  close(pipefd[1]);
  return pid;
}
