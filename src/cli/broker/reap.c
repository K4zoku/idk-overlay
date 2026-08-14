#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "internal.h"

void reap_webview(pid_t pid) {
  if (pid <= 0)
    return;
  kill(pid, SIGTERM);
  for (int i = 0; i < 20; i++) {
    int status;
    if (waitpid(pid, &status, WNOHANG) != 0)
      return;
    usleep(50000);
  }
  kill(pid, SIGKILL);
  int status;
  waitpid(pid, &status, 0);
}
