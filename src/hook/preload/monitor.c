#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"

#include "internal.h"

IDK_INTERNAL void webview_disable(void) {
  g_webview_dead = 1;
  g_overlay_visible = 0;
  g_captured = 0;
}

static int g_webview_crash_count = 0;
IDK_INTERNAL time_t g_webview_last_fork_time = 0;

/* Monitor thread — waits for the webview process to exit via waitpid.
 * Distinguishes user-close from crash using exit code + heuristic. */
IDK_INTERNAL void *webview_monitor(void *arg) {
  pid_t pid = (pid_t)(intptr_t)arg;
  int status;
  if (waitpid(pid, &status, 0) <= 0)
    return NULL;
  if (g_webview_dead)
    return NULL;
  g_webview_pid = -1;
  IDK_LOG("overlay", "webview exit: pid=%d status=0x%x\n", (int)pid, status);
  if (WIFEXITED(status))
    IDK_LOG("overlay", "  exit code=%d\n", WEXITSTATUS(status));
  if (WIFSIGNALED(status))
    IDK_LOG("overlay", "  signal=%d\n", WTERMSIG(status));
  int user_closed = 0;
  if (WIFEXITED(status)) {
    if (WEXITSTATUS(status) == 127) {
      IDK_LOG("overlay", "webview exec failed (exit=127)\n");
    } else if (WEXITSTATUS(status) == 0) {
      user_closed = 1;
    } else {
      time_t elapsed = time(NULL) - g_webview_last_fork_time;
      if (elapsed >= 2)
        user_closed = 1;
      else
        IDK_LOG("overlay", "webview exited too fast (exit=%d, %lds)\n", WEXITSTATUS(status), (long)elapsed);
    }
  } else if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    if (sig == SIGTERM || sig == SIGKILL)
      user_closed = 1;
    else
      IDK_LOG("overlay", "webview killed by signal %d\n", sig);
  }
  if (user_closed) {
    IDK_LOG("overlay", "webview (pid=%d) closed by user - overlay disabled\n", (int)pid);
    webview_disable();
    return NULL;
  }
  time_t now = time(NULL);
  if (g_webview_last_fork_time > 0 && now - g_webview_last_fork_time > 30)
    g_webview_crash_count = 0;
  g_webview_crash_count++;
  g_webview_last_fork_time = now;
  if (g_webview_crash_count > 3) {
    IDK_ERR("overlay", "webview crashed %d times - giving up, disabling overlay\n", g_webview_crash_count);
    webview_disable();
    return NULL;
  }
  IDK_LOG("overlay", "webview crashed (count=%d) - reforking\n", g_webview_crash_count);
  fork_webview();
  return NULL;
}
