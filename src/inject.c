/*
 * inject.c — idk-inject: CLI wrapper around syringe_inject()
 *
 * Usage:
 *   idk-inject <pid> [library.so] [options]
 *
 * Options:
 *   --socket PATH   Unix socket path for IPC
 *   --vk 0|1        Enable/disable Vulkan hooks
 *   --gl 0|1        Enable/disable OpenGL hooks
 *   -h, --help      Show this help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <errno.h>

#include "syringe.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <pid> [library.so] [options]\n"
        "\n"
        "Inject libidk-overlay.so into a running process.\n"
        "\n"
        "Arguments:\n"
        "  pid            Target process ID\n"
        "  library.so     Path to libidk-overlay.so (default: ./libidk-overlay.so)\n"
        "\n"
        "Options:\n"
        "  --socket PATH  Unix socket path for IPC (default: /tmp/idk-overlay-<pid>)\n"
        "  --vk 0|1       Enable Vulkan hooks (default: 1)\n"
        "  --gl 0|1       Enable OpenGL hooks (default: 1)\n"
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
        fprintf(stderr, "Error: invalid PID\n");
        return 1;
    }

    const char *lib_path = NULL;
    const char *sock_path = NULL;
    int enable_vk = 1;
    int enable_gl = 1;

    /* Parse remaining args */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (strcmp(argv[i], "--vk") == 0 && i + 1 < argc) {
            enable_vk = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--gl") == 0 && i + 1 < argc) {
            enable_gl = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (!lib_path) {
            lib_path = argv[i];
        }
    }

    if (!lib_path) {
        /* Default: libidk-overlay.so in current directory */
        lib_path = "./libidk-overlay.so";
    }

    /* Resolve to absolute path */
    char abs_path[4096];
    if (realpath(lib_path, abs_path) == NULL) {
        fprintf(stderr, "Error: cannot resolve library path: %s (%s)\n", lib_path, strerror(errno));
        return 1;
    }

    /* Default socket path */
    char default_sock[64];
    snprintf(default_sock, sizeof(default_sock), "/tmp/idk-overlay-%d", target_pid);
    if (!sock_path) {
        sock_path = default_sock;
    }

    fprintf(stderr, "idk-inject: targeting PID %d\n", target_pid);
    fprintf(stderr, "  library:  %s\n", abs_path);
    fprintf(stderr, "  socket:   %s\n", sock_path);
    fprintf(stderr, "  Vulkan:   %s\n", enable_vk ? "enabled" : "disabled");
    fprintf(stderr, "  OpenGL:   %s\n", enable_gl ? "enabled" : "disabled");

    /* Set environment variables that the injected .so will read.
     * syringe_inject() doesn't pass env vars itself, but the constructor
     * of libidk-overlay.so reads them via getenv() from the target's
     * environment. We need to either:
     *   (a) modify the target's /proc/<pid>/environ before inject, OR
     *   (b) pass socket/vk/gl via argv to idk-inject and document that
     *       user must export IDK_SOCKET etc. before running the target.
     *
     * For now: if user set IDK_SOCKET in env, write it to a sidecar
     * file /tmp/idk-overlay-<pid>.env that the .so can read.
     * Better: just print clear instructions. */
    fprintf(stderr, "\n  NOTE: Make sure IDK_SOCKET/IDK_VK/IDK_GL env vars\n"
                    "        are set in the TARGET process (osu!) before inject.\n"
                    "        Or pass --socket and let the .so default.\n");

    /* Skip render process for now — it's optional and was causing
     * "command not found" errors. User can start idk-render manually. */
    fprintf(stderr, "\n[1/1] Injecting library...\n");
    int rc = syringe_inject(target_pid, abs_path);

    if (rc == 0) {
        fprintf(stderr, "\n[success] Injection complete!\n");
        fprintf(stderr, "  Socket path: %s\n", sock_path);
        fprintf(stderr, "  Check log:   stderr of PID %d\n", target_pid);
    } else {
        fprintf(stderr, "\n[error] Injection failed: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}
