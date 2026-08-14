/* render/gl internal.h - shared state + helpers for the GL/EGL backend */

#ifndef IDK_RENDER_GL_INTERNAL_H
#define IDK_RENDER_GL_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "core/compositor.h"
#include "core/log.h"
#include "gl/gl_loader.h"
#include "hook/dlsym_shim.h"
#include "public/idk_ipc.h"

/* Internal helpers: not part of the public symbol surface. */
#define IDK_INTERNAL __attribute__((visibility("hidden")))

/* EGL types/constants (subset, no system EGL headers) */
typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLImageKHR;
typedef int32_t EGLint;
typedef uint32_t EGLenum;
typedef uint32_t EGLBoolean;
typedef intptr_t EGLNativeDisplayType;
typedef void *GLeglImage;

#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_IMAGE_KHR ((EGLImageKHR)0)
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define DRM_FORMAT_MOD_INVALID 0x00FFFFFFFFFFFFFFULL

typedef EGLDisplay (*PFN_eglGetDisplay_fn)(EGLNativeDisplayType);
typedef EGLDisplay (*PFN_eglGetCurrentDisplay_fn)(void);
typedef EGLint (*PFN_eglGetError_fn)(void);
typedef void *(*PFN_eglGetProcAddress_fn)(const char *);
typedef EGLImageKHR (*PFN_eglCreateImageKHR_fn)(EGLDisplay dpy, EGLContext ctx, EGLenum target, void *buffer,
                                                const EGLint *attrs);
typedef EGLBoolean (*PFN_eglDestroyImageKHR_fn)(EGLDisplay dpy, EGLImageKHR image);
typedef EGLBoolean (*PFN_eglInitialize_fn)(EGLDisplay dpy, EGLint *major, EGLint *minor);
typedef EGLBoolean (*PFN_eglChooseConfig_fn)(EGLDisplay dpy, const EGLint *attribs, EGLSurface *configs,
                                             EGLint config_size, EGLint *num_config);
typedef EGLSurface (*PFN_eglCreatePbufferSurface_fn)(EGLDisplay dpy, EGLSurface config, const EGLint *attribs);
typedef EGLContext (*PFN_eglCreateContext_fn)(EGLDisplay dpy, EGLSurface config, EGLContext share,
                                              const EGLint *attribs);
typedef EGLBoolean (*PFN_eglMakeCurrent_fn)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
typedef EGLBoolean (*PFN_eglDestroyContext_fn)(EGLDisplay dpy, EGLContext ctx);
typedef EGLBoolean (*PFN_eglDestroySurface_fn)(EGLDisplay dpy, EGLSurface surf);
typedef EGLBoolean (*PFN_eglBindAPI_fn)(EGLenum api);

typedef void (*PFN_glEGLImageTargetTexture2DOES_fn)(GLenum target, GLeglImage image);
typedef void (*PFN_glEGLImageTargetTexStorageEXT_fn)(GLenum target, GLeglImage image, const GLint *attrib_list);

/* EGL function pointers - resolved in resolve.c */
IDK_INTERNAL extern PFN_eglGetDisplay_fn fn_eglGetDisplay;
IDK_INTERNAL extern PFN_eglGetCurrentDisplay_fn fn_eglGetCurrentDisplay;
IDK_INTERNAL extern PFN_eglGetError_fn fn_eglGetError;
IDK_INTERNAL extern PFN_eglGetProcAddress_fn fn_eglGetProcAddress;
IDK_INTERNAL extern PFN_eglCreateImageKHR_fn fn_eglCreateImageKHR;
IDK_INTERNAL extern PFN_eglDestroyImageKHR_fn fn_eglDestroyImageKHR;
IDK_INTERNAL extern PFN_eglInitialize_fn fn_eglInitialize;
IDK_INTERNAL extern PFN_eglChooseConfig_fn fn_eglChooseConfig;
IDK_INTERNAL extern PFN_eglCreatePbufferSurface_fn fn_eglCreatePbufferSurface;
IDK_INTERNAL extern PFN_eglCreateContext_fn fn_eglCreateContext;
IDK_INTERNAL extern PFN_eglMakeCurrent_fn fn_eglMakeCurrent;
IDK_INTERNAL extern PFN_eglDestroyContext_fn fn_eglDestroyContext;
IDK_INTERNAL extern PFN_eglDestroySurface_fn fn_eglDestroySurface;
IDK_INTERNAL extern PFN_eglBindAPI_fn fn_eglBindAPI;
IDK_INTERNAL extern PFN_glEGLImageTargetTexture2DOES_fn fn_glEGLImageTargetTexture2DOES;
IDK_INTERNAL extern PFN_glEGLImageTargetTexStorageEXT_fn fn_glEGLImageTargetTexStorageEXT;

/* EGL display + GL context/program state - defined in context.c */
IDK_INTERNAL extern EGLDisplay g_egl_display;
IDK_INTERNAL extern GLuint g_program;
IDK_INTERNAL extern bool g_is_gles;
IDK_INTERNAL extern int g_gl_version;

/* Double-buffered texture slots - defined in tex.c */
IDK_INTERNAL extern GLuint g_tex[2];
IDK_INTERNAL extern int g_tex_w[2];
IDK_INTERNAL extern int g_tex_h[2];
IDK_INTERNAL extern EGLImageKHR g_tex_img[2];
IDK_INTERNAL extern int g_tex_dmabuf_fd[2];
IDK_INTERNAL extern int g_tex_idx;
IDK_INTERNAL extern uint16_t g_dmabuf_cache_id;

/* Draw error counter - defined in errors.c */
IDK_INTERNAL extern int g_draw_err_count;

/* Saved GL state around overlay drawing (state.c) */
typedef struct {
  GLenum last_active_texture;
  GLint last_program;
  GLint last_texture;
  GLint last_sampler;
  GLint last_array_buffer;
  GLint last_element_array_buffer;
  GLint last_vertex_array_object;
  GLint last_polygon_mode[2];
  GLint last_viewport[4];
  GLint last_scissor_box[4];
  GLint last_color_mask[4];
  GLenum last_blend_src_rgb;
  GLenum last_blend_dst_rgb;
  GLenum last_blend_src_alpha;
  GLenum last_blend_dst_alpha;
  GLenum last_blend_equation_rgb;
  GLenum last_blend_equation_alpha;
  GLboolean last_enable_blend;
  GLboolean last_enable_cull_face;
  GLboolean last_enable_depth_test;
  GLboolean last_enable_stencil_test;
  GLboolean last_enable_scissor_test;
  GLboolean last_srgb_enabled;
  GLboolean last_enable_primitive_restart;
  GLint last_fbo;
  GLint last_draw_buffer;
} gl_state_t;

/* resolve.c */
IDK_INTERNAL void resolve_egl_functions(void);

/* import_gl.c - GL_EXT_memory_object dmabuf import */
IDK_INTERNAL GLuint gl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t fourcc);

/* import_dmabuf.c - EGL_LINUX_DMA_BUF_EXT import.
 * Caller must keep *out_img alive for the lifetime of the returned texture. */
IDK_INTERNAL GLuint egl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t format,
                                          uint64_t modifier, EGLImageKHR *out_img);

/* tex.c */
IDK_INTERNAL void release_dmabuf_backing(int i);
IDK_INTERNAL GLuint shm_to_texture(int shm_fd, uint32_t w, uint32_t h, uint32_t pixel_size, uint32_t buffer_idx,
                                   int premultiplied);
/* Import a dmabuf frame into the back texture slot. Returns 0 on success,
 * 1 on failure (frame must be acked as failed). */
IDK_INTERNAL uint8_t gl_import_frame(const idk_frame_header_t *hdr, int fd);

/* state.c */
IDK_INTERNAL void gl_state_save(gl_state_t *st);
IDK_INTERNAL void gl_state_restore(const gl_state_t *st);

/* errors.c */
IDK_INTERNAL int gl_program_ensure_valid(void);
IDK_INTERNAL void gl_stale_frame_clear(void);
IDK_INTERNAL void gl_draw_error_check(GLint fb_w, GLint fb_h);

/* Shader loader (version.c / select.c / spirv.c / glsl.c / link.c) */

/* Shader variant selected from the GL/GLSL version (select.c). */
struct shader_variant {
  const char *ver_str;
  const char *vs_body;
  const char *fs_body;
  size_t vs_size;
  size_t fs_size;
  const unsigned char *vs_spirv;
  const unsigned char *fs_spirv;
  size_t vs_spirv_size;
  size_t fs_spirv_size;
};

/* version.c */
IDK_INTERNAL void detect_gl_version(int *out_gl_version, bool *out_is_gles);
/* select.c */
IDK_INTERNAL int glsl_version_for(int gl_version, bool is_gles);
IDK_INTERNAL void select_variant(int glsl_version, bool is_gles, struct shader_variant *v);
/* spirv.c */
IDK_INTERNAL int try_spirv(unsigned int *out_vs, unsigned int *out_fs, const unsigned char *vs_spv, size_t vs_spv_size,
                           const unsigned char *fs_spv, size_t fs_spv_size);
/* glsl.c */
IDK_INTERNAL int try_glsl(unsigned int *out_vs, unsigned int *out_fs, const char *ver_str, const char *vs_body,
                          size_t vs_size, const char *fs_body, size_t fs_size);

#endif
