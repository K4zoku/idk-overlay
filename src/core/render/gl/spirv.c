#include "core/log.h"
#include "gl/gl_loader.h"

#include "internal.h"

/* Try SPIR-V compile. Returns 1 if both vertex and fragment compile.
 * Leaves *out_vs / *out_fs zeroed on failure. */

int try_spirv(unsigned int *out_vs, unsigned int *out_fs, const unsigned char *vs_spv, size_t vs_spv_size,
              const unsigned char *fs_spv, size_t fs_spv_size) {
  int ok;

  *out_vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderBinary(1, out_vs, GL_SHADER_BINARY_FORMAT_SPIR_V, vs_spv, (GLsizei)vs_spv_size);
  glSpecializeShader(*out_vs, "main", 0, NULL, NULL);
  glCompileShader(*out_vs);
  glGetShaderiv(*out_vs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    IDK_LOG("shdr", "SPIR-V vs failed\n");
    glDeleteShader(*out_vs);
    *out_vs = 0;
    return 0;
  }

  *out_fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderBinary(1, out_fs, GL_SHADER_BINARY_FORMAT_SPIR_V, fs_spv, (GLsizei)fs_spv_size);
  glSpecializeShader(*out_fs, "main", 0, NULL, NULL);
  glCompileShader(*out_fs);
  glGetShaderiv(*out_fs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    IDK_LOG("shdr", "SPIR-V fs failed\n");
    glDeleteShader(*out_vs);
    glDeleteShader(*out_fs);
    *out_vs = 0;
    *out_fs = 0;
    return 0;
  }

  return 1;
}

/* Check SPIR-V driver support */

int idk_shader_loader_has_spirv(void) {
  if (idk_fn_glShaderBinary) {
    GLint formats[16] = {0};
    glGetIntegerv(GL_SHADER_BINARY_FORMATS, formats);
    for (int i = 0; i < 16 && formats[i]; i++) {
      if (formats[i] == GL_SHADER_BINARY_FORMAT_SPIR_V)
        return 1;
    }
  }
  return 0;
}
