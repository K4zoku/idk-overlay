/* tex.c - double-buffered overlay textures + SHM upload */

#include <unistd.h>
#include <string.h>

#include "public/idk_shm_delta.h"

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

/* The GL context belongs to the game. It may have a PBO and custom unpack
 * strides/offsets bound when the overlay hook runs. Our memfd is a CPU pointer,
 * so always isolate the unpack state and restore it after each upload. */
typedef struct shm_unpack_state {
  GLint alignment, row_length, skip_rows, skip_pixels, pixel_unpack_buffer;
} shm_unpack_state_t;

static void shm_unpack_begin(shm_unpack_state_t *st) {
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &st->alignment);
  st->row_length = st->skip_rows = st->skip_pixels = st->pixel_unpack_buffer = 0;
  if (!g_is_gles || g_gl_version >= 300) {
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &st->row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &st->skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &st->skip_pixels);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &st->pixel_unpack_buffer);
  }
  if (idk_fn_glPixelStorei) {
    idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!g_is_gles || g_gl_version >= 300) {
      idk_fn_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      idk_fn_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
      idk_fn_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    }
  }
  if (idk_fn_glBindBuffer && (!g_is_gles || g_gl_version >= 300))
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

static void shm_unpack_end(const shm_unpack_state_t *st) {
  if (idk_fn_glPixelStorei) {
    idk_fn_glPixelStorei(GL_UNPACK_ALIGNMENT, st->alignment);
    if (!g_is_gles || g_gl_version >= 300) {
      idk_fn_glPixelStorei(GL_UNPACK_ROW_LENGTH, st->row_length);
      idk_fn_glPixelStorei(GL_UNPACK_SKIP_ROWS, st->skip_rows);
      idk_fn_glPixelStorei(GL_UNPACK_SKIP_PIXELS, st->skip_pixels);
    }
  }
  if (idk_fn_glBindBuffer && (!g_is_gles || g_gl_version >= 300))
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)st->pixel_unpack_buffer);
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

  if (!idk_shm_cache_map(&s_shm_cache, shm_fd)) {
      return 0;
  }

  uint32_t buf_size = pixel_size;
  uint8_t *buf = (uint8_t *)s_shm_cache.map + (buffer_idx * buf_size);

  shm_unpack_state_t unpack;
  GLint old_texture = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  shm_unpack_begin(&unpack);

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
    idk_fn_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_tex_w[back] = (GLsizei)w;
    g_tex_h[back] = (GLsizei)h;
  } else {
    glBindTexture(GL_TEXTURE_2D, g_tex[back]);
    idk_fn_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  }

  shm_unpack_end(&unpack);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);

  g_tex_idx = back;
  g_comp.has_frame = true;
  g_comp.frame_premultiplied = (premultiplied != 0);
  g_draw_err_count = 0;

  IDK_LOG("comp", "SHM frame uploaded: %ux%u tex[%d]=%u premul=%d buf_idx=%u\n", (unsigned)w, (unsigned)h, back,
          g_tex[back], (int)g_comp.frame_premultiplied, (unsigned)buffer_idx);

  return g_tex[g_tex_idx];
}

/* Apply a packed SHM ROI to the currently displayed SHM texture. The producer
 * is forbidden to send a delta until the consumer ACKed a full SHM baseline. */
GLuint shm_rect_to_texture(int fd, uint32_t w, uint32_t h) {
  static idk_shm_cache_t cache;
  if (!idk_shm_cache_map(&cache, fd) || cache.size < sizeof(idk_shm_delta_t))
    return 0;
  idk_shm_delta_t rect;
  memcpy(&rect, cache.map, sizeof(rect));
  if (!idk_shm_delta_validate(&rect, w, h, cache.size))
    return 0;
  int slot = g_tex_idx;
  if (!g_tex[slot] || g_tex_img[slot] || g_tex_dmabuf_fd[slot] >= 0 ||
      g_tex_w[slot] != (int)w || g_tex_h[slot] != (int)h || !idk_fn_glTexSubImage2D)
    return 0;
  shm_unpack_state_t unpack;
  GLint old_texture = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  shm_unpack_begin(&unpack);
  glBindTexture(GL_TEXTURE_2D, g_tex[slot]);
  const uint8_t *pixels = (const uint8_t *)cache.map + sizeof(rect);
  idk_fn_glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)rect.x, (GLint)rect.y, (GLsizei)rect.width,
                         (GLsizei)rect.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  shm_unpack_end(&unpack);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
  if (glGetError() != GL_NO_ERROR)
    return 0;
  g_comp.has_frame = true;
  g_comp.frame_premultiplied = true;
  return g_tex[slot];
}

GLuint shm_ring_to_texture(const void *data, uint32_t w, uint32_t h, uint32_t pixel_size, int premultiplied) {
  if (!data || w == 0 || h == 0 || (uint64_t)w * h * 4 > pixel_size)
    return 0;
  const uint8_t *buf = (const uint8_t *)data;
  shm_unpack_state_t unpack;
  GLint old_texture = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  shm_unpack_begin(&unpack);
  int back = 1 - g_tex_idx;
  bool size_changed = (GLsizei)w != g_tex_w[back] || (GLsizei)h != g_tex_h[back];
  bool was_dmabuf = (g_tex_img[back] != 0) || (g_tex_dmabuf_fd[back] >= 0);
  if ((size_changed || was_dmabuf) && g_tex[back] != 0) {
    glDeleteTextures(1, &g_tex[back]);
    g_tex[back] = 0;
  }
  if (was_dmabuf)
    release_dmabuf_backing(back);
  if (g_tex[back] == 0) {
    glGenTextures(1, &g_tex[back]);
    glBindTexture(GL_TEXTURE_2D, g_tex[back]);
    idk_fn_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_tex_w[back] = (GLsizei)w;
    g_tex_h[back] = (GLsizei)h;
  } else {
    glBindTexture(GL_TEXTURE_2D, g_tex[back]);
    idk_fn_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  }
  shm_unpack_end(&unpack);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
  g_tex_idx = back;
  g_comp.has_frame = true;
  g_comp.frame_premultiplied = premultiplied != 0;
  g_draw_err_count = 0;
  return g_tex[g_tex_idx];
}

GLuint shm_ring_rect_to_texture(const void *data, size_t bytes, uint32_t w, uint32_t h) {
  if (!data || bytes < sizeof(idk_shm_delta_t))
    return 0;
  const idk_shm_delta_t *rect = (const idk_shm_delta_t *)data;
  if (!idk_shm_delta_validate(rect, w, h, bytes))
    return 0;
  int slot = g_tex_idx;
  if (!g_tex[slot] || g_tex_img[slot] || g_tex_dmabuf_fd[slot] >= 0 || g_tex_w[slot] != (int)w ||
      g_tex_h[slot] != (int)h || !idk_fn_glTexSubImage2D)
    return 0;
  shm_unpack_state_t unpack;
  GLint old_texture = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  shm_unpack_begin(&unpack);
  glBindTexture(GL_TEXTURE_2D, g_tex[slot]);
  const uint8_t *pixels = (const uint8_t *)data + sizeof(*rect);
  idk_fn_glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)rect->x, (GLint)rect->y, (GLsizei)rect->width,
                         (GLsizei)rect->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  shm_unpack_end(&unpack);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
  if (glGetError() != GL_NO_ERROR)
    return 0;
  g_comp.has_frame = true;
  g_comp.frame_premultiplied = true;
  return g_tex[slot];
}
