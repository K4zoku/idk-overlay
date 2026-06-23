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
#include <limits.h>
#include <errno.h>

#include "syringe.h"
#include "core/log.h"

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
        "  --socket PATH  Unix socket path for IPC (default: /tmp/idk-overlay-<pid>)\n"
        "  --vk 0|1       Enable Vulkan hooks (default: 1)\n"
        "  --gl 0|1       Enable OpenGL hooks (default: 1)\n"
        "  -v, --verbose  Enable verbose logging\n"
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
    const char *sock_path = NULL;
    int enable_vk = 1;
    int enable_gl = 1;
    int verbose = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (strcmp(argv[i], "--vk") == 0 && i + 1 < argc) {
            enable_vk = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--gl") == 0 && i + 1 < argc) {
            enable_gl = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (!lib_path && argv[i][0] != '-') {
            lib_path = argv[i];
        }
    }

    (void)verbose;  /* reserved for future verbose logging */

    /* Auto-detect library path */
    char abs_path[PATH_MAX];
    if (!lib_path) {
        /* Try common locations */
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

    /* Default socket path: PID-based */
    char default_sock[64];
    snprintf(default_sock, sizeof(default_sock), "/tmp/idk-overlay-%d", target_pid);
    if (!sock_path) {
        sock_path = default_sock;
    }

    IDK_LOG("inject", "targeting PID %d\n", target_pid);
    IDK_LOG("inject", "  library:  %s\n", lib_path);
    IDK_LOG("inject", "  socket:   %s\n", sock_path);
    IDK_LOG("inject", "  Vulkan:   %s\n", enable_vk ? "enabled" : "disabled");
    IDK_LOG("inject", "  OpenGL:   %s\n", enable_gl ? "enabled" : "disabled");

    /* Inject. syringe auto-detects .NET vs native. */
    IDK_LOG("inject", "[1/1] Injecting library...\n");
    int rc = syringe_inject(target_pid, lib_path);

    if (rc == 0) {
        fprintf(stderr, "[+] Injection complete!\n");
        fprintf(stderr, "    Socket: %s\n", sock_path);
        fprintf(stderr, "    Start webview: IDK_SOCKET=%s idk-webview\n", sock_path);
    } else {
        IDK_ERR("inject", "injection failed: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}
