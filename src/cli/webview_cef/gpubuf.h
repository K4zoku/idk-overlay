#pragma once
#include <cstdint>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "include/cef_render_handler.h"

/* GPU staging pipeline, mirroring the Qt webview's rhi/ path.
 *
 * The game-side compositor imports dmabufs with the driver's default
 * (tiled) layout — linear buffers get misread (correct colors, wrong
 * pixel positions). So the CEF linear dmabuf is blitted (GPU-only) into
 * a driver-native gbm buffer (GBM_BO_USE_RENDERING → Y-tiled on i915)
 * and that buffer's fd is what gets sent.
 *
 * The staging buffer is cached and reused across frames (recreated on
 * resize); the compositor keeps importing it (stable buf_id generation).
 *
 * Mesa EGL/GLES are dlopen'd by absolute path — CEF's dist pulls ANGLE's
 * libEGL.so.1 into the process, so a second copy of the system EGL is
 * needed (the probe proved this pattern).
 */
class GpuBuf {
public:
  GpuBuf() = default;
  ~GpuBuf();
  GpuBuf(const GpuBuf &) = delete;
  GpuBuf &operator=(const GpuBuf &) = delete;

  /* Lazy init (dlopen Mesa EGL/GLES + GBM device). False → caller keeps
   * the direct-forward path. */
  bool Init();

  /* Blit the CEF frame into the cached staging buffer. On success fills
   * stride/fourcc/modifier/buf_id for the frame header and returns 0.
   * Fd() is valid until the next call or a size change. */
  int BlitAndExport(const CefAcceleratedPaintInfo &info, int w, int h, uint32_t *stride, uint32_t *fourcc,
                    uint64_t *modifier, uint16_t *buf_id);

  int Fd() const { return fd_; }

private:
  bool EnsureBo(int w, int h);
  void DestroyBo();
  /* Import a dmabuf as a GL texture (destroys the previous import). */
  uint32_t ImportTexture(int fd, int w, int h, uint32_t stride, uint32_t fourcc, uint64_t modifier);
  bool WaitForGpu();

  /* dlopen'd handles (Mesa, second copy) */
  void *egl_handle_ = nullptr;
  void *gles_handle_ = nullptr;

  /* EGL */
  void *fn_get_platform_display_ = nullptr;
  void *fn_initialize_ = nullptr;
  void *fn_bind_api_ = nullptr;
  void *fn_choose_config_ = nullptr;
  void *fn_create_context_ = nullptr;
  void *fn_make_current_ = nullptr;
  void *fn_create_image_ = nullptr;
  void *fn_destroy_image_ = nullptr;
  void *fn_get_proc_address_ = nullptr;
  void *fn_get_error_ = nullptr;
  void *fn_query_string_ = nullptr;
  EGLDisplay dpy_ = nullptr;
  EGLContext ctx_ = nullptr;
  void *gbm_dev_ = nullptr;

  /* GL (resolved via eglGetProcAddress) */
  void *fn_gen_textures_ = nullptr;
  void *fn_delete_textures_ = nullptr;
  void *fn_bind_texture_ = nullptr;
  void *fn_tex_parameteri_ = nullptr;
  void *fn_gen_framebuffers_ = nullptr;
  void *fn_bind_framebuffer_ = nullptr;
  void *fn_framebuffer_texture2d_ = nullptr;
  void *fn_blit_framebuffer_ = nullptr;
  void *fn_delete_framebuffers_ = nullptr;
  void *fn_gl_get_error_ = nullptr;
  void *fn_fence_sync_ = nullptr;
  void *fn_client_wait_sync_ = nullptr;
  void *fn_delete_sync_ = nullptr;
  void *fn_flush_ = nullptr;
  void *fn_finish_ = nullptr;
  void *fn_image_target_texture2d_ = nullptr;

  /* staging buffer (cached) */
  void *bo_ = nullptr;
  int fd_ = -1;
  int bo_w_ = 0;
  int bo_h_ = 0;
  uint32_t bo_stride_ = 0;
  uint64_t bo_modifier_ = 0;
  uint16_t generation_ = 1;
  void *bo_image_ = nullptr; /* EGLImage of the bo */
  uint32_t bo_tex_ = 0;      /* GL texture bound to bo_image_ */
  uint32_t fbo_ = 0;         /* FBO targeting bo_tex_ */

  /* per-frame import */
  void *in_image_ = nullptr;
  uint32_t in_tex_ = 0;
  uint32_t in_fbo_ = 0;
};
