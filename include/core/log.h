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
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

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

#ifdef __cplusplus
extern "C" {
#endif

/* Get cached "[PID:procname]" ident string for log prefix */
const char *idk_process_ident(void);

/* Return just the process name (reuses cached ident) */
const char *idk_process_name(void);

/* Invalidate cached ident so next call re-reads /proc/self/comm.
 * Call this after prctl(PR_SET_NAME, ...) to reflect new process name. */
void idk_process_ident_invalidate(void);

#ifdef __cplusplus
}
#endif

/* Info/debug log - only prints if IDK_DEBUG is set.
 * Includes millisecond timestamp for easier debugging of timing issues. */
#define IDK_LOG(tag, fmt, ...) do { \
    if (idk_debug_enabled()) { \
        char _ts[16]; \
        idk_timestamp(_ts, sizeof(_ts)); \
        fprintf(stderr, "[%s][%s][idk:%s] " fmt, _ts, idk_process_ident(), tag, ##__VA_ARGS__); \
    } \
} while(0)

/* Error log - always prints (errors should be visible even without IDK_DEBUG) */
#define IDK_ERR(tag, fmt, ...) \
    do { \
        char _ts[16]; \
        idk_timestamp(_ts, sizeof(_ts)); \
        fprintf(stderr, "[%s][%s][idk:%s] " fmt, _ts, idk_process_ident(), tag, ##__VA_ARGS__); \
    } while(0)

#endif /* IDK_LOG_H */
