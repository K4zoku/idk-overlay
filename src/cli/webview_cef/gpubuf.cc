#include "gpubuf.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstring>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include "core/log.h"

#define DRM_FORMAT_ABGR8888 0x34324241u
#define DRM_FORMAT_ARGB8888 0x34325241u

typedef void *(*PFN_EGL_GET_PLATFORM_DISPLAY_EXT)(EGLenum, void *, const EGLint *);
typedef EGLBoolean (*PFN_EGL_INITIALIZE)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*PFN_EGL_BIND_API)(EGLenum);
typedef EGLBoolean (*PFN_EGL_CHOOSE_CONFIG)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLContext (*PFN_EGL_CREATE_CONTEXT)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*PFN_EGL_MAKE_CURRENT)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLImageKHR (*PFN_EGL_CREATE_IMAGE)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*PFN_EGL_DESTROY_IMAGE)(EGLDisplay, EGLImageKHR);
typedef void *(*PFN_EGL_GET_PROC_ADDRESS)(const char *);
typedef EGLint (*PFN_EGL_GET_ERROR)(void);
typedef const char *(*PFN_EGL_QUERY_STRING)(EGLDisplay, EGLint);

typedef void (*PFN_GL_GEN_TEXTURES)(GLsizei, GLuint *);
typedef void (*PFN_GL_DELETE_TEXTURES)(GLsizei, const GLuint *);
typedef void (*PFN_GL_BIND_TEXTURE)(GLenum, GLuint);
typedef void (*PFN_GL_TEX_PARAMETERI)(GLenum, GLenum, GLint);
typedef void (*PFN_GL_GEN_FRAMEBUFFERS)(GLsizei, GLuint *);
typedef void (*PFN_GL_BIND_FRAMEBUFFER)(GLenum, GLuint);
typedef void (*PFN_GL_FRAMEBUFFER_TEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*PFN_GL_BLIT_FRAMEBUFFER)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (*PFN_GL_DELETE_FRAMEBUFFERS)(GLsizei, const GLuint *);
typedef GLenum (*PFN_GL_GET_ERROR)(void);
typedef void (*PFN_GL_IMAGE_TARGET_TEXTURE2D)(GLenum, void *);
typedef GLsync (*PFN_GL_FENCE_SYNC)(GLenum, GLbitfield);
typedef GLenum (*PFN_GL_CLIENT_WAIT_SYNC)(GLsync, GLbitfield, GLuint64);
typedef void (*PFN_GL_DELETE_SYNC)(GLsync);
typedef void (*PFN_GL_FLUSH)(void);
typedef void (*PFN_GL_FINISH)(void);

/* Mesa EGL candidate paths (glvnd). The first that dlopens wins. */
static const char *const kEglCandidates[] = {
    "/usr/lib/libEGL.so.1.1.0",
    "/usr/lib/x86_64-linux-gnu/libEGL.so.1",
    "/usr/lib64/libEGL.so.1",
    nullptr,
};

GpuBuf::~GpuBuf() {
  DestroyBo();
  if (dpy_ != EGL_NO_DISPLAY && ctx_ != EGL_NO_CONTEXT) {
    ((PFN_EGL_MAKE_CURRENT)fn_make_current_)(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    ((PFN_EGL_DESTROY_IMAGE)fn_destroy_image_)(dpy_, bo_image_);
    ((PFN_EGL_MAKE_CURRENT)fn_make_current_)(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (bo_)
    gbm_bo_destroy((gbm_bo *)bo_);
  if (gbm_dev_)
    gbm_device_destroy((gbm_device *)gbm_dev_);
}

bool GpuBuf::Init() {
  if (dpy_ != EGL_NO_DISPLAY)
    return true;

  for (int i = 0; kEglCandidates[i]; i++) {
    egl_handle_ = dlopen(kEglCandidates[i], RTLD_NOW | RTLD_LOCAL);
    if (egl_handle_)
      break;
  }
  if (!egl_handle_) {
    IDK_LOG("webview-cef", "gpubuf: no Mesa libEGL found\n");
    return false;
  }
  fn_get_proc_address_ = dlsym(egl_handle_, "eglGetProcAddress");
  if (!fn_get_proc_address_) {
    IDK_LOG("webview-cef", "gpubuf: no eglGetProcAddress\n");
    return false;
  }
  auto GPA = [this](const char *name) { return ((PFN_EGL_GET_PROC_ADDRESS)fn_get_proc_address_)(name); };
  fn_get_platform_display_ = GPA("eglGetPlatformDisplayEXT");
  fn_initialize_ = GPA("eglInitialize");
  fn_bind_api_ = GPA("eglBindAPI");
  fn_choose_config_ = GPA("eglChooseConfig");
  fn_create_context_ = GPA("eglCreateContext");
  fn_make_current_ = GPA("eglMakeCurrent");
  fn_create_image_ = GPA("eglCreateImageKHR");
  fn_destroy_image_ = GPA("eglDestroyImageKHR");
  fn_get_error_ = GPA("eglGetError");
  fn_query_string_ = GPA("eglQueryString");
  fn_gen_textures_ = GPA("glGenTextures");
  fn_delete_textures_ = GPA("glDeleteTextures");
  fn_bind_texture_ = GPA("glBindTexture");
  fn_tex_parameteri_ = GPA("glTexParameteri");
  fn_gen_framebuffers_ = GPA("glGenFramebuffers");
  fn_bind_framebuffer_ = GPA("glBindFramebuffer");
  fn_framebuffer_texture2d_ = GPA("glFramebufferTexture2D");
  fn_blit_framebuffer_ = GPA("glBlitFramebuffer");
  fn_delete_framebuffers_ = GPA("glDeleteFramebuffers");
  fn_gl_get_error_ = GPA("glGetError");
  fn_image_target_texture2d_ = GPA("glEGLImageTargetTexture2DOES");
  fn_fence_sync_ = GPA("glFenceSync");
  fn_client_wait_sync_ = GPA("glClientWaitSync");
  fn_delete_sync_ = GPA("glDeleteSync");
  fn_flush_ = GPA("glFlush");
  fn_finish_ = GPA("glFinish");
  if (!fn_get_platform_display_ || !fn_initialize_ || !fn_bind_api_ || !fn_choose_config_ || !fn_create_context_ ||
      !fn_make_current_ || !fn_create_image_ || !fn_destroy_image_ || !fn_gen_textures_ || !fn_bind_texture_ ||
      !fn_framebuffer_texture2d_ || !fn_blit_framebuffer_) {
    IDK_LOG("webview-cef", "gpubuf: EGL/GL entrypoints incomplete\n");
    return false;
  }

  int rfd = open("/dev/dri/renderD128", O_RDWR);
  if (rfd < 0)
    rfd = open("/dev/dri/renderD129", O_RDWR);
  if (rfd < 0) {
    IDK_LOG("webview-cef", "gpubuf: no render node\n");
    return false;
  }
  gbm_dev_ = gbm_create_device(rfd);
  if (!gbm_dev_) {
    IDK_LOG("webview-cef", "gpubuf: gbm_create_device failed\n");
    return false;
  }

  dpy_ = ((PFN_EGL_GET_PLATFORM_DISPLAY_EXT)fn_get_platform_display_)(EGL_PLATFORM_GBM_MESA, gbm_dev_, nullptr);
  if (dpy_ == EGL_NO_DISPLAY || !((PFN_EGL_INITIALIZE)fn_initialize_)(dpy_, nullptr, nullptr)) {
    IDK_LOG("webview-cef", "gpubuf: EGL init on GBM failed\n");
    return false;
  }
  ((PFN_EGL_BIND_API)fn_bind_api_)(EGL_OPENGL_ES_API);
  const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
  EGLConfig cfg;
  EGLint ncfg = 0;
  if (!((PFN_EGL_CHOOSE_CONFIG)fn_choose_config_)(dpy_, cfg_attr, &cfg, 1, &ncfg) || ncfg == 0) {
    IDK_LOG("webview-cef", "gpubuf: eglChooseConfig failed\n");
    return false;
  }
  const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  ctx_ = ((PFN_EGL_CREATE_CONTEXT)fn_create_context_)(dpy_, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (ctx_ == EGL_NO_CONTEXT || !((PFN_EGL_MAKE_CURRENT)fn_make_current_)(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_)) {
    IDK_LOG("webview-cef", "gpubuf: EGL context failed\n");
    return false;
  }
  const char *vendor = ((PFN_EGL_QUERY_STRING)fn_query_string_)(dpy_, EGL_VENDOR);
  IDK_LOG("webview-cef", "gpubuf: EGL ready vendor=%s\n", vendor ? vendor : "?");
  return true;
}

/* Import a dmabuf as a GL texture. |fourcc| is the DRM format of the
 * buffer; |modifier| 0/INVALID → no modifier attrs (linear). */
uint32_t GpuBuf::ImportTexture(int fd, int w, int h, uint32_t stride, uint32_t fourcc, uint64_t modifier) {
  EGLint attrs[16];
  int ai = 0;
  attrs[ai++] = EGL_WIDTH;
  attrs[ai++] = w;
  attrs[ai++] = EGL_HEIGHT;
  attrs[ai++] = h;
  attrs[ai++] = EGL_LINUX_DRM_FOURCC_EXT;
  attrs[ai++] = (EGLint)fourcc;
  attrs[ai++] = EGL_DMA_BUF_PLANE0_FD_EXT;
  attrs[ai++] = fd;
  attrs[ai++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
  attrs[ai++] = 0;
  attrs[ai++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
  attrs[ai++] = (EGLint)stride;
  if (modifier != 0) {
    attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attrs[ai++] = (EGLint)(modifier & 0xFFFFFFFF);
    attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attrs[ai++] = (EGLint)((modifier >> 32) & 0xFFFFFFFF);
  }
  attrs[ai] = EGL_NONE;

  EGLImageKHR img =
      ((PFN_EGL_CREATE_IMAGE)fn_create_image_)(dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
  if (img == EGL_NO_IMAGE_KHR)
    return 0;

  GLuint tex = 0;
  ((PFN_GL_GEN_TEXTURES)fn_gen_textures_)(1, &tex);
  ((PFN_GL_BIND_TEXTURE)fn_bind_texture_)(GL_TEXTURE_2D, tex);
  ((PFN_GL_IMAGE_TARGET_TEXTURE2D)fn_image_target_texture2d_)(GL_TEXTURE_2D, img);
  if (((PFN_GL_GET_ERROR)fn_gl_get_error_)() != GL_NO_ERROR) {
    ((PFN_GL_DELETE_TEXTURES)fn_delete_textures_)(1, &tex);
    ((PFN_EGL_DESTROY_IMAGE)fn_destroy_image_)(dpy_, img);
    return 0;
  }
  ((PFN_GL_TEX_PARAMETERI)fn_tex_parameteri_)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  ((PFN_GL_TEX_PARAMETERI)fn_tex_parameteri_)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  ((PFN_GL_TEX_PARAMETERI)fn_tex_parameteri_)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  ((PFN_GL_TEX_PARAMETERI)fn_tex_parameteri_)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  ((PFN_GL_BIND_TEXTURE)fn_bind_texture_)(GL_TEXTURE_2D, 0);

  if (in_image_)
    ((PFN_EGL_DESTROY_IMAGE)fn_destroy_image_)(dpy_, (EGLImageKHR)in_image_);
  if (in_tex_)
    ((PFN_GL_DELETE_TEXTURES)fn_delete_textures_)(1, &in_tex_);
  in_image_ = img;
  in_tex_ = tex;
  return tex;
}

void GpuBuf::DestroyBo() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (bo_image_) {
    ((PFN_EGL_DESTROY_IMAGE)fn_destroy_image_)(dpy_, (EGLImageKHR)bo_image_);
    bo_image_ = nullptr;
  }
  if (bo_tex_)
    ((PFN_GL_DELETE_TEXTURES)fn_delete_textures_)(1, &bo_tex_);
  bo_tex_ = 0;
  if (fbo_)
    ((PFN_GL_DELETE_FRAMEBUFFERS)fn_delete_framebuffers_)(1, &fbo_);
  fbo_ = 0;
  if (bo_) {
    gbm_bo_destroy((gbm_bo *)bo_);
    bo_ = nullptr;
  }
  bo_w_ = bo_h_ = 0;
}

bool GpuBuf::EnsureBo(int w, int h) {
  if (bo_ && bo_w_ == w && bo_h_ == h)
    return true;
  DestroyBo();

  gbm_bo *bo = gbm_bo_create((gbm_device *)gbm_dev_, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING);
  if (!bo) {
    IDK_LOG("webview-cef", "gpubuf: gbm_bo_create(RENDERING) failed\n");
    return false;
  }
  bo_ = bo;
  bo_w_ = w;
  bo_h_ = h;
  fd_ = gbm_bo_get_fd(bo);
  bo_stride_ = gbm_bo_get_stride(bo);
  bo_modifier_ = gbm_bo_get_modifier(bo);
  if (generation_ == 0)
    generation_ = 1;
  if (++generation_ == 0)
    generation_ = 1;
  IDK_LOG("webview-cef", "gpubuf bo: fd=%d stride=%u mod=0x%llx gen=%u\n", fd_, bo_stride_,
          (unsigned long long)bo_modifier_, (unsigned)generation_);

  EGLint attrs[] = {EGL_WIDTH,
                    w,
                    EGL_HEIGHT,
                    h,
                    EGL_LINUX_DRM_FOURCC_EXT,
                    (EGLint)GBM_FORMAT_ABGR8888,
                    EGL_DMA_BUF_PLANE0_FD_EXT,
                    fd_,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                    0,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT,
                    (EGLint)bo_stride_,
                    EGL_NONE};
  bo_image_ = ((PFN_EGL_CREATE_IMAGE)fn_create_image_)(dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
  if (bo_image_ == EGL_NO_IMAGE_KHR) {
    IDK_LOG("webview-cef", "gpubuf: EGLImage of staging bo failed\n");
    DestroyBo();
    return false;
  }

  ((PFN_GL_GEN_TEXTURES)fn_gen_textures_)(1, &bo_tex_);
  ((PFN_GL_BIND_TEXTURE)fn_bind_texture_)(GL_TEXTURE_2D, bo_tex_);
  ((PFN_GL_IMAGE_TARGET_TEXTURE2D)fn_image_target_texture2d_)(GL_TEXTURE_2D, bo_image_);
  if (((PFN_GL_GET_ERROR)fn_gl_get_error_)() != GL_NO_ERROR) {
    IDK_LOG("webview-cef", "gpubuf: staging texture bind failed\n");
    DestroyBo();
    return false;
  }
  ((PFN_GL_TEX_PARAMETERI)fn_tex_parameteri_)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  ((PFN_GL_TEX_PARAMETERI)fn_tex_parameteri_)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  ((PFN_GL_BIND_TEXTURE)fn_bind_texture_)(GL_TEXTURE_2D, 0);

  ((PFN_GL_GEN_FRAMEBUFFERS)fn_gen_framebuffers_)(1, &fbo_);
  ((PFN_GL_BIND_FRAMEBUFFER)fn_bind_framebuffer_)(GL_FRAMEBUFFER, fbo_);
  ((PFN_GL_FRAMEBUFFER_TEXTURE2D)fn_framebuffer_texture2d_)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                                            bo_tex_, 0);
  ((PFN_GL_BIND_FRAMEBUFFER)fn_bind_framebuffer_)(GL_FRAMEBUFFER, 0);
  if (((PFN_GL_GET_ERROR)fn_gl_get_error_)() != GL_NO_ERROR) {
    IDK_LOG("webview-cef", "gpubuf: staging FBO failed\n");
    DestroyBo();
    return false;
  }
  return true;
}

bool GpuBuf::WaitForGpu() {
  if (fn_fence_sync_ && fn_client_wait_sync_ && fn_delete_sync_) {
    GLsync fence = ((PFN_GL_FENCE_SYNC)fn_fence_sync_)(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (fence) {
      ((PFN_GL_FLUSH)fn_flush_)();
      GLenum result = ((PFN_GL_CLIENT_WAIT_SYNC)fn_client_wait_sync_)(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
      ((PFN_GL_DELETE_SYNC)fn_delete_sync_)(fence);
      return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
    }
  }
  if (fn_finish_)
    ((PFN_GL_FINISH)fn_finish_)();
  return fn_finish_ != nullptr;
}

int GpuBuf::BlitAndExport(const CefAcceleratedPaintInfo &info, int w, int h, uint32_t *stride, uint32_t *fourcc,
                          uint64_t *modifier, uint16_t *buf_id) {
  if (info.plane_count < 1)
    return -1;
  const auto &p = info.planes[0];

  if (!EnsureBo(w, h))
    return -1;
  if (!ImportTexture(p.fd, w, h, p.stride,
                     info.format == CEF_COLOR_TYPE_RGBA_8888 ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_ARGB8888,
                     info.modifier))
    return -1;

  /* Single GPU blit: CEF linear (bottom-up memory) → staging
   * (driver-native tiled). Invert Y so the staging buffer is top-down,
   * matching what the compositor's quad expects. */
  if (!in_fbo_)
    ((PFN_GL_GEN_FRAMEBUFFERS)fn_gen_framebuffers_)(1, &in_fbo_);
  ((PFN_GL_BIND_FRAMEBUFFER)fn_bind_framebuffer_)(GL_READ_FRAMEBUFFER, in_fbo_);
  ((PFN_GL_FRAMEBUFFER_TEXTURE2D)fn_framebuffer_texture2d_)(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                                            in_tex_, 0);
  ((PFN_GL_BIND_FRAMEBUFFER)fn_bind_framebuffer_)(GL_DRAW_FRAMEBUFFER, fbo_);
  ((PFN_GL_BLIT_FRAMEBUFFER)fn_blit_framebuffer_)(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  ((PFN_GL_BIND_FRAMEBUFFER)fn_bind_framebuffer_)(GL_FRAMEBUFFER, 0);
  if (!WaitForGpu())
    return -1;

  *stride = bo_stride_;
  *fourcc = GBM_FORMAT_ABGR8888;
  *modifier = bo_modifier_;
  *buf_id = generation_;
  return 0;
}
