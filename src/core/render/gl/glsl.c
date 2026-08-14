#include <string.h>

#include "core/log.h"
#include "gl/gl_loader.h"

#include "internal.h"

/* Try GLSL compile. Returns 1 if both vertex and fragment compile. */

int try_glsl(unsigned int *out_vs, unsigned int *out_fs, const char *ver_str, const char *vs_body, size_t vs_size,
             const char *fs_body, size_t fs_size) {
  int ok;

  if (!idk_fn_glCreateShader || !idk_fn_glShaderSource || !idk_fn_glCompileShader || !idk_fn_glGetShaderiv) {
    IDK_ERR("shdr", "try_glsl: critical GL shader functions are NULL - cannot compile\n");
    return 0;
  }

  const GLchar *vs_src[] = {ver_str, vs_body};
  const GLint vs_len[] = {(GLint)strlen(ver_str), (GLint)vs_size};
  *out_vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(*out_vs, 2, vs_src, vs_len);
  glCompileShader(*out_vs);
  glGetShaderiv(*out_vs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLchar log[512];
    glGetShaderiv(*out_vs, GL_INFO_LOG_LENGTH, &ok);
    if (ok > 0) {
      glGetShaderInfoLog(*out_vs, 512, NULL, log);
      IDK_ERR("shdr", "VS log:\n%s\n", log);
    }
    glDeleteShader(*out_vs);
    *out_vs = 0;
    return 0;
  }

  const GLchar *fs_src[] = {ver_str, fs_body};
  const GLint fs_len[] = {(GLint)strlen(ver_str), (GLint)fs_size};
  *out_fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(*out_fs, 2, fs_src, fs_len);
  glCompileShader(*out_fs);
  glGetShaderiv(*out_fs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLchar log[512];
    glGetShaderiv(*out_fs, GL_INFO_LOG_LENGTH, &ok);
    if (ok > 0) {
      glGetShaderInfoLog(*out_fs, 512, NULL, log);
      IDK_ERR("shdr", "FS log:\n%s\n", log);
    }
    glDeleteShader(*out_vs);
    glDeleteShader(*out_fs);
    *out_vs = 0;
    *out_fs = 0;
    return 0;
  }

  return 1;
}
