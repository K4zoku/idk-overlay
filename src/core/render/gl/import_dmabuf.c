/* import_dmabuf.c - dmabuf import via EGL_LINUX_DMA_BUF_EXT */

#include "internal.h"

/* EGL image bind diagnostics (persist across frames) */
static int s_texstorage_success_logged = 0;
static int s_texstorage_failed = 0;

/* Build the EGL_LINUX_DMA_BUF_EXT attribute list. Include the modifier
 * if it's valid (not DRM_FORMAT_MOD_INVALID). Returns the index before
 * the modifier attributes (used for the no-modifier retry). */
static int egl_import_attrs(EGLint attrs[16], int dmabuf_fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t drm_fmt,
                            uint64_t modifier) {
  int ai = 0;
  attrs[ai++] = EGL_WIDTH;
  attrs[ai++] = (EGLint)w;
  attrs[ai++] = EGL_HEIGHT;
  attrs[ai++] = (EGLint)h;
  attrs[ai++] = EGL_LINUX_DRM_FOURCC_EXT;
  attrs[ai++] = (EGLint)drm_fmt;
  attrs[ai++] = EGL_DMA_BUF_PLANE0_FD_EXT;
  attrs[ai++] = dmabuf_fd;
  attrs[ai++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
  attrs[ai++] = 0;
  attrs[ai++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
  attrs[ai++] = (EGLint)stride;
  int ai_nom = ai;
  if (modifier != 0 && modifier != DRM_FORMAT_MOD_INVALID) {
    attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attrs[ai++] = (EGLint)(modifier & 0xFFFFFFFF);
    attrs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attrs[ai++] = (EGLint)((modifier >> 32) & 0xFFFFFFFF);
  }
  attrs[ai++] = EGL_NONE;
  return ai_nom;
}

/* Bind img to tex. TexStorageEXT (GL_EXT_EGL_image_storage) first - the
 * desktop-GL EGLImage path; Texture2DOES is a GLES extension, last-resort
 * only. Resolves the bind entry points from the already-loaded GL lib. */
static GLboolean egl_bind_image(GLuint tex, EGLImageKHR img) {
  if (!fn_glEGLImageTargetTexture2DOES && fn_eglGetProcAddress) {
    fn_glEGLImageTargetTexture2DOES =
        (PFN_glEGLImageTargetTexture2DOES_fn)fn_eglGetProcAddress("glEGLImageTargetTexture2DOES");
  }
  if (!fn_glEGLImageTargetTexStorageEXT && fn_eglGetProcAddress) {
    fn_glEGLImageTargetTexStorageEXT =
        (PFN_glEGLImageTargetTexStorageEXT_fn)fn_eglGetProcAddress("glEGLImageTargetTexStorageEXT");
  }

  GLboolean ok = GL_FALSE;
  GLenum err = GL_NO_ERROR;

  if (fn_glEGLImageTargetTexStorageEXT) {
    IDK_LOG("comp", "TexStorageEXT bind: tex=%u img=%p\n", tex, (void *)img);
    while (glGetError() != GL_NO_ERROR) {
    }
    fn_glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, (GLeglImage)img, NULL);
    IDK_LOG("comp", "TexStorageEXT bind returned\n");
    err = glGetError();
    if (err == GL_NO_ERROR) {
      ok = GL_TRUE;
      if (!s_texstorage_success_logged) {
        IDK_LOG("comp", "EGL image bound via TexStorageEXT (will not re-log)\n");
        s_texstorage_success_logged = 1;
      }
    } else {
      s_texstorage_failed = 1;
      IDK_LOG("comp", "TexStorageEXT failed (0x%04X)\n", err);
    }
  }

  if (!ok && fn_glEGLImageTargetTexture2DOES) {
    IDK_LOG("comp", "Texture2DOES bind: tex=%u img=%p\n", tex, (void *)img);
    while (glGetError() != GL_NO_ERROR) {
    }
    fn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImage)img);
    IDK_LOG("comp", "Texture2DOES bind returned\n");
    err = glGetError();
    if (err == GL_NO_ERROR) {
      ok = GL_TRUE;
      IDK_LOG("comp", "EGL image bound via Texture2DOES\n");
    }
  }
  if (err != GL_NO_ERROR) {
    IDK_ERR("comp", "EGL image import failed: 0x%04X\n", err);
  }
  return ok;
}

/* Import dmabuf as GL texture via EGL_LINUX_DMA_BUF_EXT. Returns texture
 * ID on success, 0 on failure. Caller must keep *out_img alive for the
 * lifetime of the returned texture. */
GLuint egl_dmabuf_to_texture(int dmabuf_fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t format, uint64_t modifier,
                             EGLImageKHR *out_img) {
  if (out_img)
    *out_img = 0;

  EGLDisplay egl_dpy = fn_eglGetCurrentDisplay ? fn_eglGetCurrentDisplay() : EGL_NO_DISPLAY;
  if (egl_dpy == EGL_NO_DISPLAY) {
    IDK_LOG("comp", "egl_dmabuf_to_texture: no current EGL display (GLX host?), using GL_EXT_memory_object\n");
    return 0;
  }

  if (!fn_eglCreateImageKHR) {
    resolve_egl_functions();
    if (!fn_eglCreateImageKHR) {
      IDK_ERR("comp", "EGL dma_buf import not available\n");
      return 0;
    }
  }

  uint32_t drm_fmt = format;
  if (drm_fmt == 0)
    drm_fmt = 0x34324742;

  EGLint attrs[16];
  int ai_nom = egl_import_attrs(attrs, dmabuf_fd, w, h, stride, drm_fmt, modifier);

  IDK_LOG("comp", "eglCreateImage: %ux%u fourcc=0x%x stride=%u modifier=0x%llx dpy=%p\n", (unsigned)w, (unsigned)h,
          (unsigned)drm_fmt, (unsigned)stride, (unsigned long long)modifier, (void *)egl_dpy);

  EGLImageKHR img = fn_eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
  IDK_LOG("comp", "eglCreateImageKHR returned %p\n", (void *)img);

  if (img == EGL_NO_IMAGE_KHR) {
    EGLint egl_err = fn_eglGetError ? fn_eglGetError() : 0;
    IDK_LOG("comp", "eglCreateImageKHR failed: 0x%04X (dpy=%p) - retrying without modifier\n", (unsigned int)egl_err,
            (void *)egl_dpy);

    if (modifier != 0 && modifier != DRM_FORMAT_MOD_INVALID) {
      attrs[ai_nom] = EGL_NONE;
      img = fn_eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
      if (img == EGL_NO_IMAGE_KHR) {
        egl_err = fn_eglGetError ? fn_eglGetError() : 0;
        IDK_ERR("comp", "eglCreateImageKHR retry (no modifier) also failed: 0x%04X\n", (unsigned int)egl_err);
      } else {
        IDK_LOG("comp", "eglCreateImageKHR retry (no modifier) OK - imported as linear\n");
      }
    }

    if (img == EGL_NO_IMAGE_KHR) {
      return 0;
    }
  }

  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  if (!egl_bind_image(tex, img)) {
    glDeleteTextures(1, &tex);
    if (fn_eglDestroyImageKHR)
      fn_eglDestroyImageKHR(egl_dpy, img);
    return 0;
  }

  if (s_texstorage_failed && !s_texstorage_success_logged) {
    IDK_LOG("comp", "WARNING: using Texture2DOES fallback (TexStorageEXT failed) - "
                    "texture may not be stable across GL contexts\n");
  }

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (out_img)
    *out_img = img;

  return tex;
}
