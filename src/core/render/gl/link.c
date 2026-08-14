#include "core/log.h"
#include "gl/gl_loader.h"
#include "gl/shader_loader.h"

#include "internal.h"

/* Link program. Takes ownership of vs/fs (deletes them), returns 0 on failure. */

static unsigned int link_program(unsigned int vs, unsigned int fs) {
  int ok;

  unsigned int prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);

  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLchar log[512];
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &ok);
    if (ok > 0) {
      glGetProgramInfoLog(prog, 512, NULL, log);
      IDK_ERR("shdr", "Link log: %s\n", log);
    }
    glDeleteProgram(prog);
    prog = 0;
  }

  glDeleteShader(vs);
  glDeleteShader(fs);
  return prog;
}

/* Compile program (SPIR-V preferred, GLSL fallback). Returns linked handle, 0 on failure. */

static unsigned int compile_program(struct shader_variant *v) {
  unsigned int vs = 0, fs = 0;

  if (v->vs_spirv && v->vs_spirv_size > 4 && v->fs_spirv && v->fs_spirv_size > 4 && idk_shader_loader_has_spirv()) {

    if (try_spirv(&vs, &fs, v->vs_spirv, v->vs_spirv_size, v->fs_spirv, v->fs_spirv_size)) {
      IDK_LOG("shdr", "SPIR-V compilation OK\n");
      return link_program(vs, fs);
    }
    IDK_LOG("shdr", "SPIR-V failed, fallback to GLSL\n");
  }

  IDK_LOG("shdr", "Using %s shader variant\n", v->ver_str);

  if (!try_glsl(&vs, &fs, v->ver_str, v->vs_body, v->vs_size, v->fs_body, v->fs_size))
    return 0;

  return link_program(vs, fs);
}

/* Public API */

unsigned int idk_shader_loader_init(int *out_gl_version, bool *out_is_gles) {
  int gl_version;
  bool is_gles;

  detect_gl_version(&gl_version, &is_gles);

  if (out_gl_version)
    *out_gl_version = gl_version;
  if (out_is_gles)
    *out_is_gles = is_gles;

  int glsl_ver = glsl_version_for(gl_version, is_gles);
  struct shader_variant v = {0};
  select_variant(glsl_ver, is_gles, &v);

  return compile_program(&v);
}
