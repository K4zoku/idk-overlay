#ifndef IDK_DLSYM_SHIM_H
#define IDK_DLSYM_SHIM_H

/* Bypass our dlsym() interposition — resolves the real libc functions. */
void *real_dlsym(void *handle, const char *symbol);
void *real_dlopen(const char *filename, int flags);
char *real_dlerror(void);

/* Resolve real libc pointers eagerly. MUST be called from the overlay
 * constructor before wine starts loading DLLs. */
void idk_shim_init(void);

#endif /* IDK_DLSYM_SHIM_H */
