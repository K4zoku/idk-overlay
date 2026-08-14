/*
 * idk-inject: CLI wrapper around syringe_inject()
 *
 * Usage:
 *   idk-inject <pid> [library.so] [options]
 *
 * Options:
 *   -h, --help      Show this help
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "syringe.h"

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s <pid> [library.so] [options]\n"
          "\n"
          "Inject libidk-overlay.so into a running process.\n"
          "\n"
          "Arguments:\n"
          "  pid            Target process ID\n"
          "  library.so     Path to libidk-overlay.so (default: auto-detect)\n"
          "\n"
          "Options:\n"
          "  -h, --help     Show this help\n",
          prog);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  pid_t target_pid = (pid_t)atoi(argv[1]);
  if (target_pid <= 0) {
    IDK_ERR("inject", "invalid PID\n");
    return 1;
  }

  const char *lib_path = NULL;

  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else if (!lib_path && argv[i][0] != '-') {
      lib_path = argv[i];
    }
  }

  char abs_path[PATH_MAX];
  if (!lib_path) {
    const char *candidates[] = {
        "./build/libidk-overlay.so",
        "./libidk-overlay.so",
        "/usr/lib/libidk-overlay.so",
        "/usr/local/lib/libidk-overlay.so",
        NULL,
    };
    for (int i = 0; candidates[i]; i++) {
      if (realpath(candidates[i], abs_path) != NULL) {
        lib_path = abs_path;
        break;
      }
    }
    if (!lib_path) {
      IDK_ERR("inject", "cannot find libidk-overlay.so. Specify path explicitly.\n");
      return 1;
    }
  } else {
    if (realpath(lib_path, abs_path) == NULL) {
      IDK_ERR("inject", "cannot resolve library path: %s (%s)\n", lib_path, strerror(errno));
      return 1;
    }
    lib_path = abs_path;
  }

  IDK_LOG("inject", "targeting PID %d\n", target_pid);
  IDK_LOG("inject", "  library:  %s\n", lib_path);

  IDK_LOG("inject", "[1/1] Injecting library...\n");
  int rc = syringe_inject(target_pid, lib_path);

  if (rc == 0) {
    IDK_ERR("inject", "Injection complete\n");
  } else {
    IDK_ERR("inject", "injection failed: %s\n", strerror(errno));
    return 1;
  }

  return 0;
}
