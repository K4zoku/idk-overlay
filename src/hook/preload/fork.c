#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"

#include "internal.h"

/* Locate the webview binary. Priority:
 *   1. IDK_WEBVIEW_BIN env var (explicit path)
 *   2. PATH search for "idk-webview"
 * Returns 0 and fills buf on success, -1 on failure. */
IDK_INTERNAL int find_webview_bin(char *buf, size_t bufsz) {
  const char *env = getenv("IDK_WEBVIEW_BIN");
  if (env && env[0]) {
    snprintf(buf, bufsz, "%s", env);
    return 0;
  }
  const char *path = getenv("PATH");
  if (!path)
    path = "/usr/local/bin:/usr/bin:/bin";
  const char *p = path;
  while (*p) {
    const char *colon = strchr(p, ':');
    size_t len = colon ? (size_t)(colon - p) : strlen(p);
    if (len > 0 && len < PATH_MAX - 32) {
      snprintf(buf, bufsz, "%.*s/idk-webview", (int)len, p);
      if (access(buf, X_OK) == 0)
        return 0;
    }
    if (!colon)
      break;
    p = colon + 1;
  }
  return -1;
}

/* Fork+exec the webview process. Uses syscall(SYS_execve, ...). If exec
 * fails in a Wine-isolated environment, the child exits with code 127
 * and the monitor will refork. */
IDK_INTERNAL void fork_webview(void) {
  char bin[PATH_MAX];
  if (find_webview_bin(bin, sizeof(bin)) != 0) {
    IDK_LOG("overlay", "webview binary not found (set IDK_WEBVIEW_BIN or install idk-webview in PATH)\n");
    return;
  }
  if (g_input_eventfd < 0) {
    g_input_eventfd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
    if (g_input_eventfd >= 0) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", g_input_eventfd);
      setenv("IDK_INPUT_EVENTFD", buf, 1);
    }
  }
  g_webview_last_fork_time = time(NULL);
  g_webview_pid = fork();
  if (g_webview_pid < 0) {
    IDK_ERR("overlay", "fork() failed: %s\n", strerror(errno));
    return;
  }
  if (g_webview_pid == 0) {
    const char *comm = idk_process_name();
    for (int i = 3; i < 1024; i++) {
      if (i == g_input_eventfd)
        continue;
      close(i);
    }
    extern char **environ;
    for (int ei = 0; environ[ei]; ei++) {
      if (strncmp(environ[ei], "LD_PRELOAD=", 11) == 0) {
        char *val = environ[ei] + 11;
        if (!*val)
          break;
        char *newval = val, *w = val;
        while (*newval) {
          while (*newval == ':' || *newval == ' ')
            newval++;
          if (!*newval)
            break;
          char *start = newval;
          while (*newval && *newval != ':')
            newval++;
          if ((size_t)(newval - start) > 0 && !strstr(start, "libidk-overlay.so")) {
            if (w > val)
              *w++ = ':';
            size_t slen = (size_t)(newval - start);
            memmove(w, start, slen);
            w += slen;
          }
        }
        *w = '\0';
        if (w == val) {
          for (int ej = ei; environ[ej]; ej++)
            environ[ej] = environ[ej + 1];
        }
        break;
      }
    }
    char *argv[] = {bin, "--socket", g_socket_path, "--match", (char *)comm, NULL};
    IDK_LOG("overlay", "forked webview child, exec %s (comm=%s socket=%s)\n", bin, comm[0] ? comm : "", g_socket_path);
    syscall(SYS_execve, bin, argv, environ);
    _exit(127);
  }
  IDK_LOG("overlay", "webview forked (pid=%d, bin=%s)\n", (int)g_webview_pid, bin);
  pthread_t t;
  if (pthread_create(&t, NULL, webview_monitor, (void *)(intptr_t)g_webview_pid) == 0)
    pthread_detach(t);
}
