/*
 * idk_log.h - Conditional logging for idk-overlay
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
#include <time.h>

/* Check IDK_DEBUG once at startup, cache the result */
static inline int idk_debug_enabled(void) {
    static int cached = -1;
    if (cached == -1) {
        cached = (getenv("IDK_DEBUG") != NULL);
    }
    return cached;
}

/* Get millisecond timestamp string for log prefix: [HH:MM:SS.mmm] */
static inline void idk_timestamp(char *buf, size_t len) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    snprintf(buf, len, "%02d:%02d:%02d.%03d",
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
}

/* Info/debug log - only prints if IDK_DEBUG is set.
 * Includes millisecond timestamp for easier debugging of timing issues. */
#define IDK_LOG(tag, fmt, ...) do { \
    if (idk_debug_enabled()) { \
        char _ts[16]; \
        idk_timestamp(_ts, sizeof(_ts)); \
        fprintf(stderr, "[%s][idk:%s] " fmt, _ts, tag, ##__VA_ARGS__); \
    } \
} while(0)

/* Error log - always prints (errors should be visible even without IDK_DEBUG) */
#define IDK_ERR(tag, fmt, ...) \
    do { \
        char _ts[16]; \
        idk_timestamp(_ts, sizeof(_ts)); \
        fprintf(stderr, "[%s][idk:%s] " fmt, _ts, tag, ##__VA_ARGS__); \
    } while(0)

#endif /* IDK_LOG_H */
