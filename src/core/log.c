#include "core/log.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static char g_ident[80] = {0};
static int g_cached = 0;

const char *idk_process_ident(void) {
    if (!g_cached) {
        char name[64] = {0};
        int fd = open("/proc/self/comm", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, name, sizeof(name) - 1);
            if (n > 0) { name[n] = '\0'; char *nl = strchr(name, '\n'); if (nl) *nl = '\0'; }
            close(fd);
        }
        snprintf(g_ident, sizeof(g_ident), "%d:%s", getpid(), name);
        g_cached = 1;
    }
    return g_ident;
}

const char *idk_process_name(void) {
    const char *ident = idk_process_ident();
    const char *colon = strchr(ident, ':');
    return colon ? colon + 1 : ident;
}

void idk_process_ident_invalidate(void) {
    g_cached = 0;
}
