/*
 * idk_log.h — Conditional logging for idk-overlay
 *
 * All log output is suppressed unless IDK_DEBUG env var is set.
 * This keeps the injected process stderr clean for normal operation.
 *
 * Usage:
 *   IDK_LOG("compositor", "frame: %ux%u\n", w, h);
 *   IDK_ERR("failed to bind socket: %s\n", strerror(errno));
 *
 * Enable: IDK_DEBUG=1 ./game
 * Disable (default): no output
 */

#ifndef IDK_LOG_H
#define IDK_LOG_H

#include <stdio.h>
#include <stdlib.h>

/* Check IDK_DEBUG once at startup, cache the result */
static inline int idk_debug_enabled(void) {
    static int cached = -1;
    if (cached == -1) {
        cached = (getenv("IDK_DEBUG") != NULL);
    }
    return cached;
}

/* Info/debug log — only prints if IDK_DEBUG is set */
#define IDK_LOG(tag, fmt, ...) do { \
    if (idk_debug_enabled()) \
        fprintf(stderr, "[idk:%s] " fmt, tag, ##__VA_ARGS__); \
} while(0)

/* Error log — always prints (errors should be visible even without IDK_DEBUG) */
#define IDK_ERR(tag, fmt, ...) \
    fprintf(stderr, "[idk:%s] " fmt, tag, ##__VA_ARGS__)

#endif /* IDK_LOG_H */
