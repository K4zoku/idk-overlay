#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

/* Wine helper processes that inherit LD_PRELOAD. */
static const char *const k_wine_blacklist[] = {
    "wineboot",     "wineserver",   "services.exe", "explorer.exe", "svchost.exe",     "winedevice.exe",
    "mscorsvw.exe", "plugplay.exe", "rpcss.exe",    "msiexec.exe",  "rundll32.exe",    "cmd.exe",
    "reg.exe",      "schtasks.exe", "start.exe",    "bridge.exe",   "_v2-entry-point", NULL,
};

/* Short-lived shell tools that inherit LD_PRELOAD from the parent. */
static const char *const k_shell_blacklist[] = {
    "awk",      "gawk",    "mawk",     "sh",   "bash", "dash", "UpdateNix", "mktemp", "cp",   "mv",    "rm", "chmod",
    "readlink", "dirname", "basename", "head", "tail", "cut",  "grep",      "sed",    "true", "false", NULL,
};

/* Skip overlay init in known non-game child processes that inherit
 * LD_PRELOAD from the parent. The overlay must keep LD_PRELOAD set so
 * the real game binary still receives injection. */
IDK_INTERNAL int idk_is_target_process(void) {
  char exe[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len > 0) {
    exe[len] = '\0';
    if (strstr(exe, "/.mount_"))
      return 1;
  }
  char cmdline[4096] = {0};
  int cmd_fd = open("/proc/self/cmdline", O_RDONLY);
  if (cmd_fd >= 0) {
    ssize_t cn = read(cmd_fd, cmdline, sizeof(cmdline) - 1);
    close(cmd_fd);
    if (cn > 0) {
      cmdline[cn] = '\0';
      for (ssize_t i = 0; i < cn; i++)
        if (cmdline[i] == '\0' && i + 1 < cn)
          cmdline[i] = ' ';
      char *lower = cmdline;
      for (char *p = lower; *p; p++)
        *p = tolower((unsigned char)*p);
      for (int i = 0; k_wine_blacklist[i]; i++)
        if (strstr(lower, k_wine_blacklist[i]))
          return 0;
    }
  }
  char comm[64] = {0};
  int _fd = open("/proc/self/comm", O_RDONLY);
  if (_fd < 0)
    return 1;
  ssize_t _n = read(_fd, comm, sizeof(comm) - 1);
  close(_fd);
  if (_n <= 0)
    return 1;
  comm[_n] = '\0';
  if (_n > 0 && comm[_n - 1] == '\n')
    comm[_n - 1] = '\0';
  for (int i = 0; k_shell_blacklist[i]; i++)
    if (strcmp(comm, k_shell_blacklist[i]) == 0)
      return 0;
  return 1;
}
