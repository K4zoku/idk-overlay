/* context.c - EGL display + GL resource lifecycle */

#include <dlfcn.h>

#include "core/compositor_egl.h"
#include "gl/shader_loader.h"
#include "internal.h"

extern void *idk_x11_game_display(void);

/* Our own EGL display (created in preload) + GL program/context info */
EGLDisplay g_egl_display = EGL_NO_DISPLAY;
GLuint g_program = 0;
bool g_is_gles = false;
int g_gl_version = 0;

PFN_glEGLImageTargetTexture2DOES_fn fn_glEGLImageTargetTexture2DOES = NULL;
PFN_glEGLImageTargetTexStorageEXT_fn fn_glEGLImageTargetTexStorageEXT = NULL;

/* Init our own EGL display from the hook install thread (egl_hook
 * idk_egl_init) BEFORE the game renders — never from the render path.
 * The display MUST be the game's X display (wine's X connection). */
void idk_compositor_egl_preload(void) {
  resolve_egl_functions();
  if (fn_eglGetDisplay && fn_eglInitialize && g_egl_display == EGL_NO_DISPLAY) {
    void *xdisplay = idk_x11_game_display();
    if (!xdisplay)
      xdisplay = NULL;
    g_egl_display = fn_eglGetDisplay((EGLNativeDisplayType)xdisplay);
    if (g_egl_display != EGL_NO_DISPLAY) {
      EGLint major = 0, minor = 0;
      if (fn_eglInitialize(g_egl_display, &major, &minor))
        IDK_LOG("comp", "EGL display initialized: %d.%d (dpy=%p)\n", (int)major, (int)minor, xdisplay);
      else
        g_egl_display = EGL_NO_DISPLAY;
    }
  }
}

/* Game surface size (shared in g_comp) */
void idk_compositor_egl_notify_resize(int w, int h) {
  bool changed = idk_comp_notify_resize(&g_comp.game_w, &g_comp.game_h, &g_comp.size_pending, &g_comp.last_resize_ts, w,
                                        h, "comp");
  if (changed) {
    IDK_LOG("comp", "resize: keeping stale texture %dx%d for display until next frame\n", g_tex_w[g_tex_idx],
            g_tex_h[g_tex_idx]);
  }
}

void idk_compositor_egl_shutdown(void) {
  idk_compositor_shutdown();
  g_program = 0;
  g_tex[0] = g_tex[1] = 0;
  g_dmabuf_cache_id = 0;
  release_dmabuf_backing(0);
  release_dmabuf_backing(1);
  g_comp.has_frame = false;
  IDK_LOG("comp", "Shut down\n");
}

/* Init shaders and GL resources */
int idk_compositor_egl_init_gl(void) {
  if (idk_gl_loader_init() != 0) {
    IDK_ERR("comp", "GL loader init failed - cannot init GL resources\n");
    return -1;
  }

  void *libgl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
  if (!libgl)
    libgl = dlopen("libGL.so.1", RTLD_NOW);
  if (!libgl)
    libgl = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (!libgl)
    libgl = dlopen("libOpenGL.so.0", RTLD_NOW);
  if (!libgl)
    libgl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_NOLOAD);
  if (!libgl)
    libgl = dlopen("libGLESv2.so.2", RTLD_NOW);
  if (!libgl)
    libgl = dlopen("libGL.so", RTLD_NOW);
  if (libgl) {
    fn_glEGLImageTargetTexture2DOES = (PFN_glEGLImageTargetTexture2DOES_fn)dlsym(libgl, "glEGLImageTargetTexture2DOES");
    IDK_LOG("comp", "glEGLImageTargetTexture2DOES dlsym=%p (libgl)\n", (void *)fn_glEGLImageTargetTexture2DOES);
    fn_glEGLImageTargetTexStorageEXT =
        (PFN_glEGLImageTargetTexStorageEXT_fn)dlsym(libgl, "glEGLImageTargetTexStorageEXT");
    IDK_LOG("comp", "glEGLImageTargetTexStorageEXT dlsym=%p (libgl)\n", (void *)fn_glEGLImageTargetTexStorageEXT);

    void *(*glxGetProcAddress)(const unsigned char *) =
        (void *(*)(const unsigned char *))dlsym(libgl, "glXGetProcAddress");
    if (glxGetProcAddress) {
      void *p = glxGetProcAddress((const unsigned char *)"glEGLImageTargetTexStorageEXT");
      if (p)
        fn_glEGLImageTargetTexStorageEXT = (PFN_glEGLImageTargetTexStorageEXT_fn)p;
      p = glxGetProcAddress((const unsigned char *)"glEGLImageTargetTexture2DOES");
      if (p)
        fn_glEGLImageTargetTexture2DOES = (PFN_glEGLImageTargetTexture2DOES_fn)p;
      IDK_LOG("comp", "glXGetProcAddress: TexStorageEXT=%p Texture2DOES=%p\n", (void *)fn_glEGLImageTargetTexStorageEXT,
              (void *)fn_glEGLImageTargetTexture2DOES);
    }
  }
  if (idk_fn_glGetString) {
    const GLubyte *rend = idk_fn_glGetString(GL_RENDERER);
    IDK_LOG("comp", "GL_RENDERER: %s\n", rend ? (const char *)rend : "(null)");
  }
  g_program = idk_shader_loader_init(&g_gl_version, &g_is_gles);
  if (g_program == 0) {
    IDK_ERR("comp", "Shader init failed - cannot init GL resources\n");
    return -1;
  }

  IDK_LOG("comp", "GL compositor ready (program=%u)\n", g_program);
  return 0;
}
