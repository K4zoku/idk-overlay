#include "gl/shader.h"

#include "internal.h"

/* Map GL version → GLSL version */

int glsl_version_for(int gl_version, bool is_gles) {
  if (!is_gles) {
    if (gl_version >= 410)
      return 410;
    if (gl_version >= 320)
      return 150;
    if (gl_version >= 300)
      return 130;
    return 120;
  } else {
    if (gl_version >= 300)
      return 300;
    return 100;
  }
}

/* Select shader variant by GLSL version */

void select_variant(int glsl_version, bool is_gles, struct shader_variant *v) {
  if (glsl_version <= 120) {
    v->ver_str = is_gles ? "#version 100\n" : "#version 120\n";
    v->vs_body = glsl_overlay_vertex_120;
    v->vs_size = GLSL_SHADER_SIZE(vertex_120);
    v->fs_body = glsl_overlay_fragment_120;
    v->fs_size = GLSL_SHADER_SIZE(fragment_120);
#ifdef HAS_SPV_120
    v->vs_spirv = spv_vertex_120;
    v->vs_spirv_size = SPV_SHADER_SIZE(vertex_120);
    v->fs_spirv = spv_fragment_120;
    v->fs_spirv_size = SPV_SHADER_SIZE(fragment_120);
#endif
  } else if (glsl_version == 300) {
    v->ver_str = "#version 300 es\n";
    v->vs_body = glsl_overlay_vertex_300_es;
    v->vs_size = GLSL_SHADER_SIZE(vertex_300_es);
    v->fs_body = glsl_overlay_fragment_300_es;
    v->fs_size = GLSL_SHADER_SIZE(fragment_300_es);
#ifdef HAS_SPV_300_es
    v->vs_spirv = spv_vertex_300_es;
    v->vs_spirv_size = SPV_SHADER_SIZE(vertex_300_es);
    v->fs_spirv = spv_fragment_300_es;
    v->fs_spirv_size = SPV_SHADER_SIZE(fragment_300_es);
#endif
  } else if (glsl_version >= 410) {
    v->ver_str = "#version 410 core\n";
    v->vs_body = glsl_overlay_vertex_410;
    v->vs_size = GLSL_SHADER_SIZE(vertex_410);
    v->fs_body = glsl_overlay_fragment_410;
    v->fs_size = GLSL_SHADER_SIZE(fragment_410);
#ifdef HAS_SPV_410
    v->vs_spirv = spv_vertex_410;
    v->vs_spirv_size = SPV_SHADER_SIZE(vertex_410);
    v->fs_spirv = spv_fragment_410;
    v->fs_spirv_size = SPV_SHADER_SIZE(fragment_410);
#endif
  } else {
    if (glsl_version >= 330)
      v->ver_str = "#version 330 core\n";
    else if (glsl_version >= 150)
      v->ver_str = "#version 150\n";
    else
      v->ver_str = "#version 130\n";
    v->vs_body = glsl_overlay_vertex_130;
    v->vs_size = GLSL_SHADER_SIZE(vertex_130);
    v->fs_body = glsl_overlay_fragment_130;
    v->fs_size = GLSL_SHADER_SIZE(fragment_130);
#ifdef HAS_SPV_130
    v->vs_spirv = spv_vertex_130;
    v->vs_spirv_size = SPV_SHADER_SIZE(vertex_130);
    v->fs_spirv = spv_fragment_130;
    v->fs_spirv_size = SPV_SHADER_SIZE(fragment_130);
#endif
  }
}
