/* GLX swap hook for overlay compositing. */

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/compositor.h"
#include "core/compositor_egl.h"
#include "core/log.h"
#include "gl/gl_loader.h"
#include "hook/dlsym_shim.h"
#include "hook/hook_plugin.h"
#include "hook/hook_util.h"
#include "hook/overlay.h"
#include "hook/syringe_hook.h"

extern _Atomic int g_overlay_visible;

typedef void *Display;
typedef void *GLXDrawable;
typedef void (*GlXSwapBuffersFn)(Display *, GLXDrawable);
typedef void *(*GlXGetProcAddressFn)(const unsigned char *);
typedef void *(*GlXGetProcAddressARBFn)(const unsigned char *);

static GlXSwapBuffersFn orig_glXSwapBuffers = NULL;
static GlXGetProcAddressFn orig_glXGetProcAddress = NULL;
static GlXGetProcAddressARBFn orig_glXGetProcAddressARB = NULL;
static int g_hook_installed = 0;
static int g_gl_resources_ready = 0;
static pthread_mutex_t g_hook_mutex = PTHREAD_MUTEX_INITIALIZER;

static GlXSwapBuffersFn resolve_real_glXSwapBuffers(void);
void glXSwapBuffers(Display *dpy, GLXDrawable drawable);

static GlXSwapBuffersFn resolve_real_glXSwapBuffers(void) {
  /* NOLOAD: libGL is loaded before any swap; RTLD_NEXT misses wine's
   * private handle, and RTLD_NOW load deadlocks (glibc vs wine locks). */
  void *lib = real_dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
  if (!lib)
    lib = real_dlopen("libGL.so", RTLD_NOW | RTLD_NOLOAD);
  if (!lib)
    return NULL;
  GlXSwapBuffersFn fn = (GlXSwapBuffersFn)real_dlsym(lib, "glXSwapBuffers");
  if (fn && fn != (GlXSwapBuffersFn)(void *)glXSwapBuffers)
    return fn;
  return NULL;
}

void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
  if (!orig_glXSwapBuffers)
    orig_glXSwapBuffers = resolve_real_glXSwapBuffers();
  if (!orig_glXSwapBuffers || orig_glXSwapBuffers == (GlXSwapBuffersFn)(void *)glXSwapBuffers)
    return;

  if (!g_gl_resources_ready) {
    if (idk_compositor_egl_init_gl() == 0)
      g_gl_resources_ready = 1;
  }

  idk_compositor_egl_render();

  if (g_overlay_visible && idk_compositor_egl_has_overlay() && idk_fn_glGetIntegerv) {
    GLint vp[4] = {0, 0, 0, 0};
    idk_fn_glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] > 0 && vp[3] > 0) {
      idk_compositor_egl_notify_resize((int)vp[2], (int)vp[3]);
      idk_compositor_egl_render_overlay(0, 0, (uint32_t)vp[2], (uint32_t)vp[3]);
    }
  }

  orig_glXSwapBuffers(dpy, drawable);
}

void *glXGetProcAddress(const unsigned char *procName) {
  if (!orig_glXGetProcAddress) {
    void *lib = real_dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
      lib = real_dlopen("libGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib)
      orig_glXGetProcAddress = (GlXGetProcAddressFn)real_dlsym(lib, "glXGetProcAddress");
  }
  if (strcmp((const char *)procName, "glXSwapBuffers") == 0)
    return (void *)glXSwapBuffers;
  if (orig_glXGetProcAddress)
    return orig_glXGetProcAddress(procName);
  return NULL;
}

void *glXGetProcAddressARB(const unsigned char *procName) {
  if (!orig_glXGetProcAddressARB) {
    void *lib = real_dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
      lib = real_dlopen("libGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib)
      orig_glXGetProcAddressARB = (GlXGetProcAddressARBFn)real_dlsym(lib, "glXGetProcAddressARB");
  }
  if (strcmp((const char *)procName, "glXSwapBuffers") == 0)
    return (void *)glXSwapBuffers;
  if (orig_glXGetProcAddressARB)
    return orig_glXGetProcAddressARB(procName);
  return NULL;
}

static int install_glx_hook(void) {
  pthread_mutex_lock(&g_hook_mutex);
  if (g_hook_installed) {
    pthread_mutex_unlock(&g_hook_mutex);
    return 0;
  }

  idk_compositor_init();

  GlXSwapBuffersFn real_fn = resolve_real_glXSwapBuffers();

  if (idk_is_wine()) {
    if (real_fn) {
      orig_glXSwapBuffers = real_fn;
      g_hook_installed = 1;
      IDK_LOG("glx", "hook installed via LD_PRELOAD + glXGetProcAddress intercept (Wine mode)\n");
      pthread_mutex_unlock(&g_hook_mutex);
      return 0;
    }
  }

  int n = syringe_hook_install("glXSwapBuffers", (void *)glXSwapBuffers, (void **)&orig_glXSwapBuffers);
  if (n > 0) {
    if (orig_glXSwapBuffers == (void *)glXSwapBuffers) {
      IDK_LOG("glx", "syringe returned self-referencing orig, overriding with libGL addr=%p\n", (void *)real_fn);
      orig_glXSwapBuffers = real_fn;
    }
    if (orig_glXSwapBuffers && orig_glXSwapBuffers != (void *)glXSwapBuffers) {
      g_hook_installed = 1;
      IDK_LOG("glx", "syringe hook installed (GOT)\n");
      pthread_mutex_unlock(&g_hook_mutex);
      return 0;
    }
  }

  orig_glXSwapBuffers = (GlXSwapBuffersFn)hook_orig("glXSwapBuffers");
  if (orig_glXSwapBuffers && orig_glXSwapBuffers != (void *)glXSwapBuffers) {
    g_hook_installed = 1;
    IDK_LOG("glx", "LD_PRELOAD mode\n");
    pthread_mutex_unlock(&g_hook_mutex);
    return 0;
  }

  IDK_LOG("glx", "hook install failed\n");
  pthread_mutex_unlock(&g_hook_mutex);
  return -1;
}

int idk_glx_init(void) {
  if (g_hook_installed)
    return 0;
  return install_glx_hook();
}

void idk_glx_shutdown(void) {
  if (!g_hook_installed)
    return;
  if (!idk_is_wine())
    syringe_hook_remove("glXSwapBuffers");
  g_hook_installed = 0;
  g_gl_resources_ready = 0;
}

idk_hook_plugin_t idk_plugin_glx = {
    .name = "glx",
    .lib_patterns = {"libGL.so.1", "libGL.so", NULL},
    .init = idk_glx_init,
    .shutdown = idk_glx_shutdown,
};
