/* errors.c - GL error handling: program re-init, stale frames, draw errors */

#include <time.h>

#include "core/compositor_egl.h"
#include "internal.h"

/* Draw error counter - reset on new frame upload, invalidate tex at 5 */
int g_draw_err_count = 0;

/* Validate the overlay program is still linked; on link loss, clear it
 * and re-initialize shaders. Returns 0 if the frame must be skipped. */
int gl_program_ensure_valid(void) {
  GLboolean is_prog = glIsProgram(g_program);
  GLint link_status = 0;
  if (is_prog)
    glGetProgramiv(g_program, GL_LINK_STATUS, &link_status);
  if (!is_prog || !link_status) {
    static int s_reinit_count = 0;
    s_reinit_count++;
    IDK_LOG("comp", "program %u invalidated (is_prog=%d link=%d) - re-initializing shaders (attempt %d)\n", g_program,
            (int)is_prog, (int)link_status, s_reinit_count);
    g_program = 0;
    if (idk_compositor_egl_init_gl() != 0) {
      IDK_ERR("comp", "shader re-init failed - skipping frame\n");
      return 0;
    }
    IDK_LOG("comp", "shader re-init OK, new g_program=%u\n", g_program);
  }
  return 1;
}

/* Drop the displayed frame if no new frame arrived for >1s. */
void gl_stale_frame_clear(void) {
  if (g_comp.has_frame) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - g_comp.last_frame_ts.tv_sec) + (now.tv_nsec - g_comp.last_frame_ts.tv_nsec) / 1e9;
    if (elapsed > 1.0) {
      IDK_LOG("comp", "stale frame cleared (%.0fms since last frame)\n", elapsed * 1000);
      g_comp.has_frame = false;
    }
  }
}

/* Post-draw error check: throttle logging (first 5, then every 300th)
 * and invalidate the texture after 5 consecutive errors. */
void gl_draw_error_check(GLint fb_w, GLint fb_h) {
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    g_draw_err_count++;
    if (g_draw_err_count <= 5 || g_draw_err_count % 300 == 0) {
      IDK_ERR("comp", "GL error after draw: 0x%x (tex[%d]=%u fb=%dx%d frame=%ux%u, occurrence %d)\n", err, g_tex_idx,
              g_tex[g_tex_idx], fb_w, fb_h, g_comp.frame_w, g_comp.frame_h, g_draw_err_count);
    }
    if (g_draw_err_count == 5) {
      GLuint dead = g_tex[g_tex_idx];
      IDK_ERR("comp", "tex[%d]=%u failing repeatedly - deleting + marking invalid\n", g_tex_idx, dead);
      if (dead > 0)
        glDeleteTextures(1, &dead);
      g_tex[g_tex_idx] = 0;
      release_dmabuf_backing(g_tex_idx);
      g_comp.has_frame = false;
    }
  }
  GLuint cur = g_tex[g_tex_idx];
  if (cur > 0 && !glIsTexture(cur)) {
    IDK_ERR("comp", "WARNING: tex[%d] (%u) is not a valid texture!\n", g_tex_idx, cur);
    g_tex[g_tex_idx] = 0;
    release_dmabuf_backing(g_tex_idx);
    g_comp.has_frame = false;
  }
}
