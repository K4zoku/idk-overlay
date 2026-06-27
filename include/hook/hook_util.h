#ifndef IDK_HOOK_UTIL_H
#define IDK_HOOK_UTIL_H

#include <dlfcn.h>

/* Resolve the original (next) function via RTLD_NEXT. Safe to call from
 * any context - returns the real implementation that was overridden. */
static inline void *hook_orig(const char *sym) {
    return dlsym(RTLD_NEXT, sym);
}

#endif /* IDK_HOOK_UTIL_H */
