/*
 * idk_gl_fns.h - GL function pointer table (X-macro)
 *
 * Defines the GL types, the GL_FOREACH X-macro listing every function
 * resolved at runtime via dlsym, and the idk_fn_* extern declarations.
 * Included by gl_loader.h (umbrella) - consumers include "gl/gl_loader.h".
 */
#ifndef IDK_GL_FNS_H
#define IDK_GL_FNS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL types (subset, enough for compositor.c) */

typedef uint32_t GLenum;
typedef int GLint;
typedef int GLintptr;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef unsigned int GLbitfield;
typedef void GLvoid;

/* GL function pointers — X-macro pattern */

#define GL_FOREACH(F)                                                                                                  \
  F(void, glGetIntegerv, (GLenum, GLint *))                                                                            \
  F(void, glEnable, (GLenum))                                                                                          \
  F(void, glDisable, (GLenum))                                                                                         \
  F(void, glBlendFunc, (GLenum, GLenum))                                                                               \
  F(GLuint, glCreateShader, (GLenum))                                                                                  \
  F(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                                     \
  F(void, glCompileShader, (GLuint))                                                                                   \
  F(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                                                    \
  F(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                                  \
  F(void, glDeleteShader, (GLuint))                                                                                    \
  F(GLuint, glCreateProgram, (void))                                                                                   \
  F(void, glAttachShader, (GLuint, GLuint))                                                                            \
  F(void, glLinkProgram, (GLuint))                                                                                     \
  F(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                                                   \
  F(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                                 \
  F(void, glDeleteProgram, (GLuint))                                                                                   \
  F(void, glUseProgram, (GLuint))                                                                                      \
  F(void, glGenTextures, (GLsizei, GLuint *))                                                                          \
  F(void, glBindTexture, (GLenum, GLuint))                                                                             \
  F(void, glTexParameteri, (GLenum, GLenum, GLint))                                                                    \
  F(void, glDeleteTextures, (GLsizei, const GLuint *))                                                                 \
  F(GLint, glGetUniformLocation, (GLuint, const GLchar *))                                                             \
  F(void, glUniform1i, (GLint, GLint))                                                                                 \
  F(void, glActiveTexture, (GLenum))                                                                                   \
  F(void, glGenBuffers, (GLsizei, GLuint *))                                                                           \
  F(void, glBindBuffer, (GLenum, GLuint))                                                                              \
  F(void, glBufferData, (GLenum, GLsizei, const GLvoid *, GLenum))                                                     \
  F(void, glBufferSubData, (GLenum, GLintptr, GLsizei, const GLvoid *))                                                \
  F(void, glDeleteBuffers, (GLsizei, const GLuint *))                                                                  \
  F(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                                      \
  F(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *))               \
  F(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *))            \
  F(void, glPixelStorei, (GLenum, GLint))                                                                              \
  F(void, glEnableVertexAttribArray, (GLuint))                                                                         \
  F(void, glDisableVertexAttribArray, (GLuint))                                                                        \
  F(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid *))                          \
  F(GLint, glGetAttribLocation, (GLuint, const GLchar *))                                                              \
  F(GLboolean, glIsEnabled, (GLenum))                                                                                  \
  F(GLboolean, glIsTexture, (GLuint))                                                                                  \
  F(GLboolean, glIsProgram, (GLuint))                                                                                  \
  F(GLenum, glGetError, (void))                                                                                        \
  F(void, glFinish, (void))                                                                                            \
  F(void, glClear, (GLbitfield))                                                                                       \
  F(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                          \
  F(void, glDrawBuffer, (GLenum))                                                                                      \
  F(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                       \
  F(void, glBlendEquation, (GLenum))                                                                                   \
  F(void, glBlendEquationSeparate, (GLenum, GLenum))                                                                   \
  F(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                                                 \
  F(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                                                \
  F(const GLubyte *, glGetString, (GLenum))                                                                            \
  F(void, glDepthMask, (GLboolean))                                                                                    \
  F(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                                   \
  F(void, glGetVertexAttribiv, (GLuint, GLenum, GLint *))                                                              \
  F(void, glGenVertexArrays, (GLsizei, GLuint *))                                                                      \
  F(void, glBindVertexArray, (GLuint))                                                                                 \
  F(void, glDeleteVertexArrays, (GLsizei, const GLuint *))                                                             \
  F(void, glBindSampler, (GLuint, GLuint))                                                                             \
  F(void, glPolygonMode, (GLenum, GLenum))                                                                             \
  F(void, glBindFramebuffer, (GLenum, GLuint))                                                                         \
  F(void, glShaderBinary, (GLsizei, const GLuint *, GLenum, const GLvoid *, GLsizei))                                  \
  F(void, glSpecializeShader, (GLuint, const GLchar *, GLuint, const GLuint *, const GLuint *))

#define GL_TYPEDEF(ret, name, params) typedef ret(*PFN_idk_##name) params;
GL_FOREACH(GL_TYPEDEF)
#undef GL_TYPEDEF

#define GL_EXTERN(ret, name, params) extern PFN_idk_##name idk_fn_##name;
GL_FOREACH(GL_EXTERN)
#undef GL_EXTERN

#ifdef __cplusplus
}
#endif

#endif /* IDK_GL_FNS_H */
