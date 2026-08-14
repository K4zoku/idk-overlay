#ifndef IDK_HOOK_UTIL_H
#define IDK_HOOK_UTIL_H

#include "hook/dlsym_shim.h"
#include <dlfcn.h>

static inline void *hook_orig(const char *sym) { return real_dlsym(RTLD_NEXT, sym); }

#endif /* IDK_HOOK_UTIL_H */
