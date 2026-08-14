/* state.c - GL state save/restore around overlay drawing */

#include "internal.h"

/* Save the GL state we mutate while drawing the overlay. Sets the active
 * texture to TEXTURE0 (the draw path binds the frame texture there). */
void gl_state_save(gl_state_t *st) {
  glGetIntegerv(GL_ACTIVE_TEXTURE, (GLint *)&st->last_active_texture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_CURRENT_PROGRAM, &st->last_program);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &st->last_texture);

  st->last_sampler = 0;
  if (!g_is_gles && g_gl_version >= 330)
    glGetIntegerv(GL_SAMPLER_BINDING, &st->last_sampler);

  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &st->last_array_buffer);
  st->last_element_array_buffer = 0;
  if (g_gl_version >= 300)
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &st->last_element_array_buffer);

  st->last_vertex_array_object = 0;
  if (g_gl_version >= 300)
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &st->last_vertex_array_object);

  st->last_polygon_mode[0] = st->last_polygon_mode[1] = GL_FILL;
  if (!g_is_gles && g_gl_version >= 200)
    glGetIntegerv(GL_POLYGON_MODE, st->last_polygon_mode);

  glGetIntegerv(GL_VIEWPORT, st->last_viewport);
  glGetIntegerv(GL_SCISSOR_BOX, st->last_scissor_box);

  st->last_color_mask[0] = st->last_color_mask[1] = st->last_color_mask[2] = st->last_color_mask[3] = GL_TRUE;
  glGetIntegerv(GL_COLOR_WRITEMASK, st->last_color_mask);

  glGetIntegerv(GL_BLEND_SRC_RGB, (GLint *)&st->last_blend_src_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, (GLint *)&st->last_blend_dst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, (GLint *)&st->last_blend_src_alpha);
  glGetIntegerv(GL_BLEND_DST_ALPHA, (GLint *)&st->last_blend_dst_alpha);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, (GLint *)&st->last_blend_equation_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, (GLint *)&st->last_blend_equation_alpha);
  st->last_enable_blend = glIsEnabled(GL_BLEND);
  st->last_enable_cull_face = glIsEnabled(GL_CULL_FACE);
  st->last_enable_depth_test = glIsEnabled(GL_DEPTH_TEST);
  st->last_enable_stencil_test = glIsEnabled(GL_STENCIL_TEST);
  st->last_enable_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
  st->last_srgb_enabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  st->last_enable_primitive_restart =
      (!g_is_gles && g_gl_version >= 310) ? glIsEnabled(GL_PRIMITIVE_RESTART) : GL_FALSE;

  st->last_fbo = -1;
  if (g_gl_version >= 300)
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &st->last_fbo);

  st->last_draw_buffer = GL_BACK;
  glGetIntegerv(GL_DRAW_BUFFER, &st->last_draw_buffer);

  while (glGetError() != GL_NO_ERROR) {
  }
}

/* Restore the GL state captured by gl_state_save. */
void gl_state_restore(const gl_state_t *st) {
  glUseProgram(st->last_program);
  glBindTexture(GL_TEXTURE_2D, st->last_texture);

  if (!g_is_gles && g_gl_version >= 330)
    glBindSampler(0, (GLuint)st->last_sampler);

  glActiveTexture(st->last_active_texture);

  if (g_gl_version >= 300)
    glBindVertexArray((GLuint)st->last_vertex_array_object);

  glBindBuffer(GL_ARRAY_BUFFER, st->last_array_buffer);
  if (g_gl_version >= 300 && st->last_element_array_buffer >= 0)
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)st->last_element_array_buffer);
  glBlendEquationSeparate(st->last_blend_equation_rgb, st->last_blend_equation_alpha);
  glBlendFuncSeparate(st->last_blend_src_rgb, st->last_blend_dst_rgb, st->last_blend_src_alpha,
                      st->last_blend_dst_alpha);
  if (st->last_enable_blend)
    glEnable(GL_BLEND);
  else
    glDisable(GL_BLEND);
  if (st->last_enable_cull_face)
    glEnable(GL_CULL_FACE);
  else
    glDisable(GL_CULL_FACE);
  if (st->last_enable_depth_test)
    glEnable(GL_DEPTH_TEST);
  else
    glDisable(GL_DEPTH_TEST);
  if (st->last_enable_stencil_test)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);
  if (st->last_enable_scissor_test)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  if (!g_is_gles && g_gl_version >= 310) {
    if (st->last_enable_primitive_restart)
      glEnable(GL_PRIMITIVE_RESTART);
  }
  if (!g_is_gles && g_gl_version >= 200)
    glPolygonMode(GL_FRONT_AND_BACK, (GLenum)st->last_polygon_mode[0]);

  glViewport(st->last_viewport[0], st->last_viewport[1], (GLsizei)st->last_viewport[2], (GLsizei)st->last_viewport[3]);
  glScissor(st->last_scissor_box[0], st->last_scissor_box[1], (GLsizei)st->last_scissor_box[2],
            (GLsizei)st->last_scissor_box[3]);

  if (st->last_srgb_enabled)
    glEnable(GL_FRAMEBUFFER_SRGB);
  glDrawBuffer((GLenum)st->last_draw_buffer);
  if (st->last_fbo >= 0)
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)st->last_fbo);

  glColorMask((GLboolean)st->last_color_mask[0], (GLboolean)st->last_color_mask[1], (GLboolean)st->last_color_mask[2],
              (GLboolean)st->last_color_mask[3]);
}
