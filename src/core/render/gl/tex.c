/* tex.c - double-buffered overlay textures + SHM upload */

#include <unistd.h>

#include "internal.h"

/* Double-buffered overlay textures. Frame N renders from slot A; frame
 * N+1 uploads to slot B (the back slot) then renders from slot B.
 *
 * For DMABUF textures we MUST keep the EGLImage and dmabuf_fd alive for
 * the texture's lifetime. */
GLuint g_tex[2] = {0, 0};
int g_tex_w[2] = {0, 0};
int g_tex_h[2] = {0, 0};
EGLImageKHR g_tex_img[2] = {0, 0};
int g_tex_dmabuf_fd[2] = {-1, -1};
int g_tex_idx = 0;
uint16_t g_dmabuf_cache_id = 0;

/* Free the DMABUF backing (EGLImage + fd) for slot i, if any.
 * Safe to call when slot i has a SHM texture (no-op). */
void release_dmabuf_backing(int i) {
  if (i < 0 || i > 1)
    return;
  if (g_tex_img[i] && fn_eglDestroyImageKHR) {
    if (g_egl_display != EGL_NO_DISPLAY)
      fn_eglDestroyImageKHR(g_egl_display, g_tex_img[i]);
  }
  g_tex_img[i] = 0;
  if (g_tex_dmabuf_fd[i] >= 0) {
    close(g_tex_dmabuf_fd[i]);
    g_tex_dmabuf_fd[i] = -1;
  }
}

/* Import a dmabuf frame (EGL path first, GL_EXT_memory_object fallback)
 * into the back texture slot, keeping the EGLImage + fd alive for the
 * texture's lifetime. Returns 0 on success, 1 on failure. */
uint8_t gl_import_frame(const idk_frame_header_t *hdr, int fd) {
  if (!fn_eglGetCurrentDisplay)
    resolve_egl_functions();
  GLuint tex = 0;
  EGLImageKHR img = 0;
  int fd_consumed = 0;

  tex = egl_dmabuf_to_texture(fd, hdr->width, hdr->height, hdr->stride, hdr->fourcc, hdr->modifier, &img);
  if (tex == 0) {
    IDK_LOG("comp", "gl_import: w=%u h=%u stride=%u fourcc=0x%x modifier=0x%llx\n", hdr->width, hdr->height,
            hdr->stride, hdr->fourcc, (unsigned long long)hdr->modifier);
    tex = gl_dmabuf_to_texture(fd, hdr->width, hdr->height, hdr->stride, hdr->fourcc);
    if (tex) {
      close(fd);
    }
  } else {
    fd_consumed = 1;
  }

  if (tex == 0) {
    if (fd >= 0)
      close(fd);
    return 1;
  }

  int back = 1 - g_tex_idx;
  if (g_tex[back])
    glDeleteTextures(1, &g_tex[back]);
  release_dmabuf_backing(back);
  g_tex[back] = tex;
  g_tex_img[back] = img;
  g_tex_dmabuf_fd[back] = fd_consumed ? fd : -1;
  g_tex_w[back] = (GLsizei)hdr->width;
  g_tex_h[back] = (GLsizei)hdr->height;
  g_tex_idx = back;
  g_dmabuf_cache_id = hdr->buf_id;
  g_comp.has_frame = true;
  g_comp.frame_premultiplied = true;
  g_draw_err_count = 0;
  return 0;
}

/* SHM → GL texture upload via glTex(Sub)Image2D */
GLuint shm_to_texture(int shm_fd, uint32_t w, uint32_t h, uint32_t pixel_size, uint32_t buffer_idx, int premultiplied) {
  if (!idk_fn_glTexImage2D) {
    IDK_ERR("comp", "glTexImage2D not resolved\n");
    return 0;
  }

  if (w == 0 || h == 0) {
    IDK_ERR("comp", "shm_to_texture: rejecting zero-dim frame w=%u h=%u\n", w, h);
    return 0;
  }
  uint32_t expected = w * h * 4;
  if (pixel_size < expected) {
    IDK_ERR("comp", "shm_to_texture: size mismatch w=%u h=%u pixel_size=%u expected=%u\n", w, h, pixel_size, expected);
    return 0;
  }
  static idk_shm_cache_t s_shm_cache;
  static int s_cached_fd = -1;

  if (shm_fd != s_cached_fd) {
    idk_shm_cache_map(&s_shm_cache, shm_fd);
    if (s_cached_fd >= 0)
      close(s_cached_fd);
    s_cached_fd = shm_fd;
  }

  uint32_t buf_size = pixel_size;
  uint8_t *buf = (uint8_t *)s_shm_cache.map + (buffer_idx * buf_size);

  GLint last_unpack_align = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &last_unpack_align);

  int back = 1 - g_tex_idx;

  bool size_changed = (GLsizei)w != g_tex_w[back] || (GLsizei)h != g_tex_h[back];
  bool was_dmabuf = (g_tex_img[back] != 0) || (g_tex_dmabuf_fd[back] >= 0);
  if ((size_changed || was_dmabuf) && g_tex[back] != 0) {
    glDeleteTextures(1, &g_tex[back]);
    g_tex[back] = 0;
  }
  if (was_dmabuf) {
    release_dmabuf_backing(back);
  }

  if (g_tex[back] == 0) {
    glGenTextures(1, &g_tex[back]);
    glBindTexture(GL_TEXTURE_2D, g_tex[back]);
    if (idk_fn_glPixelStorei) {
      idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    idk_fn_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_tex_w[back] = (GLsizei)w;
    g_tex_h[back] = (GLsizei)h;
  } else {
    glBindTexture(GL_TEXTURE_2D, g_tex[back]);
    if (idk_fn_glPixelStorei) {
      idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    idk_fn_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  }

  if (idk_fn_glPixelStorei) {
    idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, last_unpack_align);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  g_tex_idx = back;
  g_comp.has_frame = true;
  g_comp.frame_premultiplied = (premultiplied != 0);
  g_draw_err_count = 0;

  IDK_LOG("comp", "SHM frame uploaded: %ux%u tex[%d]=%u premul=%d buf_idx=%u\n", (unsigned)w, (unsigned)h, back,
          g_tex[back], (int)g_comp.frame_premultiplied, (unsigned)buffer_idx);

  return g_tex[g_tex_idx];
}
