/* render.c - frame receive + overlay draw */

#include <stdatomic.h>
#include <unistd.h>

#include "core/compositor_egl.h"
#include "internal.h"

/* Overlay visibility - same symbol as overlay.c's g_overlay_visible.
 * When 0, we drain incoming frames without ACK/REQUEST. */
extern _Atomic int g_overlay_visible;
extern _Atomic int g_webview_dead;

int idk_compositor_egl_render(void) {
  if (g_webview_dead)
    return -1;

  int rc = idk_compositor_recv_frame(g_overlay_visible);
  if (rc <= 0)
    return -1;

  idk_frame_header_t *hdr = &g_comp.hdr;
  int fd = g_comp.dmabuf_fd[0];
  uint8_t ack = 0;

  if (!idk_frame_is_dmabuf(hdr)) {
    uint32_t pixel_size = hdr->width * hdr->height * 4;
    GLuint tex = shm_to_texture(fd, hdr->width, hdr->height, pixel_size, 0, 1);
    if (tex == 0) {
      ack = 1;
      goto done;
    }
    g_dmabuf_cache_id = 0;
  } else if (hdr->buf_id != 0 && hdr->buf_id == g_dmabuf_cache_id) {
    if (fd >= 0) {
      close(fd);
      g_comp.dmabuf_fd[0] = -1;
    }
    g_comp.has_frame = true;
    g_comp.frame_premultiplied = true;
    g_draw_err_count = 0;
  } else {
    ack = gl_import_frame(hdr, fd);
    if (ack)
      goto done;
  }
  g_comp.dmabuf_fd[0] = -1;

done:
  idk_compositor_send_ack(ack);
  return ack ? -1 : 0;
}

int idk_compositor_egl_has_overlay(void) { return g_comp.has_frame ? 1 : 0; }

static void gl_blend_setup(void);
static void gl_draw_setup(GLint fb_w, GLint fb_h, GLuint *vao_out);

void idk_compositor_egl_render_overlay(int x, int y, uint32_t w, uint32_t h) {
  (void)x;
  (void)y;
  if (g_program == 0)
    return;

  if (!gl_program_ensure_valid())
    return;

  gl_stale_frame_clear();
  if (!g_comp.has_frame || g_tex[g_tex_idx] == 0)
    return;

  GLint fb_w = (GLint)w, fb_h = (GLint)h;
  {
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    fb_w = vp[2];
    fb_h = vp[3];
  }
  if (fb_w <= 0 || fb_h <= 0)
    return;

  gl_state_t st;
  gl_state_save(&st);

  GLuint vao = 0;
  gl_draw_setup(fb_w, fb_h, &vao);

  glDrawArrays(GL_TRIANGLES, 0, 6);
  gl_draw_error_check(fb_w, fb_h);

  if (vao)
    glDeleteVertexArrays(1, &vao);

  gl_state_restore(&st);
}

#define GLCHECK(label)                                                                                                 \
  do {                                                                                                                 \
    if (g_draw_err_count == 1) {                                                                                       \
      GLenum _e = glGetError();                                                                                        \
      if (_e != GL_NO_ERROR)                                                                                           \
        IDK_ERR("comp", "GL setup error @ %s: 0x%x\n", label, _e);                                                     \
    }                                                                                                                  \
  } while (0)

/* Blend mode depends on frame format:
 * - Straight alpha: src=GL_SRC_ALPHA, dst=GL_ONE_MINUS_SRC_ALPHA
 * - Premultiplied:  src=GL_ONE, dst=GL_ONE_MINUS_SRC_ALPHA */
static void gl_blend_setup(void) {
  glEnable(GL_BLEND);
  GLCHECK("glEnable(GL_BLEND)");
  glBlendEquation(GL_FUNC_ADD);
  GLCHECK("glBlendEquation");
  if (g_comp.frame_premultiplied) {
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  } else {
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  }
  GLCHECK("glBlendFuncSeparate");
}

/* Set up GL state for the overlay quad: bind the frame texture, use the
 * overlay program, and reset the fixed-function state we rely on. */
static void gl_draw_setup(GLint fb_w, GLint fb_h, GLuint *vao_out) {
  gl_blend_setup();
  glDisable(GL_CULL_FACE);
  GLCHECK("glDisable(GL_CULL_FACE)");
  glDisable(GL_DEPTH_TEST);
  GLCHECK("glDisable(GL_DEPTH_TEST)");
  glDisable(GL_STENCIL_TEST);
  GLCHECK("glDisable(GL_STENCIL_TEST)");
  glEnable(GL_SCISSOR_TEST);
  GLCHECK("glEnable(GL_SCISSOR_TEST)");
  glScissor(0, 0, fb_w, fb_h);
  GLCHECK("glScissor");
  if (g_gl_version >= 300) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLCHECK("glBindFramebuffer");
  }
  glDisable(GL_FRAMEBUFFER_SRGB);
  GLCHECK("glDisable(GL_FRAMEBUFFER_SRGB)");
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  GLCHECK("glColorMask");

  if (!g_is_gles) {
    if (g_gl_version >= 200) {
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      GLCHECK("glPolygonMode");
    }
    if (g_gl_version >= 310) {
      glDisable(GL_PRIMITIVE_RESTART);
      GLCHECK("glDisable(GL_PRIMITIVE_RESTART)");
    }
  }

  glViewport(0, 0, fb_w, fb_h);
  GLCHECK("glViewport");
  glDrawBuffer(GL_BACK);
  GLCHECK("glDrawBuffer");

  GLuint vertex_array_object = 0;
  if (g_gl_version >= 300)
    glGenVertexArrays(1, &vertex_array_object);
  if (vertex_array_object) {
    glBindVertexArray(vertex_array_object);
    GLCHECK("glBindVertexArray");
  }

  if (g_draw_err_count == 1) {
    GLuint cur_tex = g_tex[g_tex_idx];
    GLboolean tex_valid = cur_tex ? glIsTexture(cur_tex) : GL_FALSE;
    GLint prog_link = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &prog_link);
    IDK_LOG("comp", "pre-draw diag: tex[%d]=%u valid=%d prog=%u link=%d frame=%ux%u fb=%dx%d\n", g_tex_idx, cur_tex,
            (int)tex_valid, g_program, (int)prog_link, g_comp.frame_w, g_comp.frame_h, fb_w, fb_h);
  }

  glUseProgram(g_program);
  GLCHECK("glUseProgram");
  GLint loc = glGetUniformLocation(g_program, "u_texture");
  GLCHECK("glGetUniformLocation");
  glUniform1i(loc, 0);
  GLCHECK("glUniform1i");
  glActiveTexture(GL_TEXTURE0);
  GLCHECK("glActiveTexture");
  glBindTexture(GL_TEXTURE_2D, g_tex[g_tex_idx]);
  GLCHECK("glBindTexture");
  *vao_out = vertex_array_object;
}
#undef GLCHECK
