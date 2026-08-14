/* dlsym() interposition (MangoHud shim pattern). */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hook/dlsym_shim.h"

static void *(*real_dlsym_fn)(void *, const char *) = NULL;
static void *(*real_dlopen_fn)(const char *, int) = NULL;
static char *(*real_dlerror_fn)(void) = NULL;

/* Resolve real libc functions from the constructor before wine loads DLLs. */
void idk_shim_init(void) {
  if (real_dlsym_fn)
    return;
  real_dlsym_fn = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
  real_dlopen_fn = (void *(*)(const char *, int))dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5");
  real_dlerror_fn = (char *(*)(void))dlvsym(RTLD_NEXT, "dlerror", "GLIBC_2.2.5");
}

void *real_dlsym(void *handle, const char *symbol) {
  if (!real_dlsym_fn)
    idk_shim_init();
  return real_dlsym_fn(handle, symbol);
}

void *real_dlopen(const char *filename, int flags) {
  if (!real_dlopen_fn)
    idk_shim_init();
  return real_dlopen_fn(filename, flags);
}

char *real_dlerror(void) {
  if (!real_dlerror_fn)
    idk_shim_init();
  return real_dlerror_fn();
}

/* Functions we export as hooks. When a dlsym lookup for one of these
 * names succeeds (real symbol exists), return our interposed version. */

void *glXGetProcAddress(const unsigned char *procName);
void *glXGetProcAddressARB(const unsigned char *procName);
void glXSwapBuffers(void *dpy, void *drawable);
unsigned int eglSwapBuffers(void *dpy, void *surface);

static const struct {
  const char *name;
  void *ptr;
} g_shim_hooks[] = {
    {"glXGetProcAddress", (void *)glXGetProcAddress},
    {"glXGetProcAddressARB", (void *)glXGetProcAddressARB},
    {"glXSwapBuffers", (void *)glXSwapBuffers},
    {"eglSwapBuffers", (void *)eglSwapBuffers},
};

void *dlsym(void *handle, const char *name) {
  if (!name)
    return real_dlsym(handle, name);

  void *fn_ptr = real_dlsym(handle, name);
  if (fn_ptr) {
    for (size_t i = 0; i < sizeof(g_shim_hooks) / sizeof(g_shim_hooks[0]); i++) {
      if (strcmp(name, g_shim_hooks[i].name) == 0)
        return g_shim_hooks[i].ptr;
    }
  }
  return fn_ptr;
}
