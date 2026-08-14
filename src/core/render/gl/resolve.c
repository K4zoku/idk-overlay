/* resolve.c - EGL function resolution (bypasses hooked dlsym) */

#include <dlfcn.h>

#include "internal.h"

/* EGL function pointers (shared, see internal.h) */
PFN_eglGetDisplay_fn fn_eglGetDisplay = NULL;
PFN_eglGetCurrentDisplay_fn fn_eglGetCurrentDisplay = NULL;
PFN_eglGetError_fn fn_eglGetError = NULL;
PFN_eglGetProcAddress_fn fn_eglGetProcAddress = NULL;
PFN_eglCreateImageKHR_fn fn_eglCreateImageKHR = NULL;
PFN_eglDestroyImageKHR_fn fn_eglDestroyImageKHR = NULL;
PFN_eglInitialize_fn fn_eglInitialize = NULL;
PFN_eglChooseConfig_fn fn_eglChooseConfig = NULL;
PFN_eglCreatePbufferSurface_fn fn_eglCreatePbufferSurface = NULL;
PFN_eglCreateContext_fn fn_eglCreateContext = NULL;
PFN_eglMakeCurrent_fn fn_eglMakeCurrent = NULL;
PFN_eglDestroyContext_fn fn_eglDestroyContext = NULL;
PFN_eglDestroySurface_fn fn_eglDestroySurface = NULL;
PFN_eglBindAPI_fn fn_eglBindAPI = NULL;

void resolve_egl_functions(void) {
  if (fn_eglGetDisplay)
    return;
  void *lib = real_dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
  if (!lib)
    lib = real_dlopen("libEGL.so.1", RTLD_NOW);
  if (!lib)
    lib = real_dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
  if (!lib)
    lib = real_dlopen("libEGL.so", RTLD_NOW);
  if (!lib) {
    IDK_LOG("comp", "libEGL not found, EGL dmabuf path disabled\n");
    return;
  }
  fn_eglGetDisplay = (PFN_eglGetDisplay_fn)real_dlsym(lib, "eglGetDisplay");
  fn_eglGetCurrentDisplay = (PFN_eglGetCurrentDisplay_fn)real_dlsym(lib, "eglGetCurrentDisplay");
  fn_eglGetError = (PFN_eglGetError_fn)real_dlsym(lib, "eglGetError");
  fn_eglGetProcAddress = (PFN_eglGetProcAddress_fn)real_dlsym(lib, "eglGetProcAddress");
  fn_eglInitialize = (PFN_eglInitialize_fn)real_dlsym(lib, "eglInitialize");
  fn_eglChooseConfig = (PFN_eglChooseConfig_fn)real_dlsym(lib, "eglChooseConfig");
  fn_eglCreatePbufferSurface = (PFN_eglCreatePbufferSurface_fn)real_dlsym(lib, "eglCreatePbufferSurface");
  fn_eglCreateContext = (PFN_eglCreateContext_fn)real_dlsym(lib, "eglCreateContext");
  fn_eglMakeCurrent = (PFN_eglMakeCurrent_fn)real_dlsym(lib, "eglMakeCurrent");
  fn_eglDestroyContext = (PFN_eglDestroyContext_fn)real_dlsym(lib, "eglDestroyContext");
  fn_eglDestroySurface = (PFN_eglDestroySurface_fn)real_dlsym(lib, "eglDestroySurface");
  fn_eglBindAPI = (PFN_eglBindAPI_fn)real_dlsym(lib, "eglBindAPI");

  if (fn_eglGetProcAddress) {
    fn_eglCreateImageKHR = (PFN_eglCreateImageKHR_fn)fn_eglGetProcAddress("eglCreateImageKHR");
    fn_eglDestroyImageKHR = (PFN_eglDestroyImageKHR_fn)fn_eglGetProcAddress("eglDestroyImageKHR");
  }

  if (!fn_eglCreateImageKHR)
    fn_eglCreateImageKHR = (PFN_eglCreateImageKHR_fn)real_dlsym(lib, "eglCreateImageKHR");
  if (!fn_eglDestroyImageKHR)
    fn_eglDestroyImageKHR = (PFN_eglDestroyImageKHR_fn)real_dlsym(lib, "eglDestroyImageKHR");

  IDK_LOG("comp", "EGL functions: eglGetDisplay=%p eglCreateImageKHR=%p\n", (void *)fn_eglGetDisplay,
          (void *)fn_eglCreateImageKHR);
}
