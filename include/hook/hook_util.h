#ifndef IDK_HOOK_UTIL_H
#define IDK_HOOK_UTIL_H

#include <dlfcn.h>
#include <stdbool.h>

/* Check if we were loaded via LD_PRELOAD by comparing RTLD_DEFAULT resolution
 * of `sym` against `our_func`. With LD_PRELOAD the dynamic linker resolves
 * our symbol first; with late injection the real symbol is already in place. */
static inline bool hook_is_ld_preload(const char *sym, void *our_func) {
    return dlsym(RTLD_DEFAULT, sym) == our_func;
}

/* Resolve the original (next) function via RTLD_NEXT. Safe to call from
 * any context — returns the real implementation that was overridden. */
static inline void *hook_orig(const char *sym) {
    return dlsym(RTLD_NEXT, sym);
}

#endif /* IDK_HOOK_UTIL_H */
