/*
 * idk_gl_loader.h - Runtime GL symbol resolution
 *
 * Pattern: all GL functions are resolved at runtime via dlsym, never
 * linked at build time. This lets libidk-overlay.so inject into any
 * process regardless of which GL implementation it uses (Mesa GL,
 * Mesa GLES, NVIDIA, etc.) - we just grab whatever is already loaded.
 *
 * Usage:
 *   1. #include "idk_gl_loader.h" in your .c file
 *   2. Call idk_gl_loader_init() once, from a GL context
 *   3. Use GL functions as normal (macros redirect to function pointers)
 *
 * The macros below redirect direct calls like glGetIntegerv(...) to
 * (*idk_fn_glGetIntegerv)(...) - so existing code that calls GL functions
 * by name doesn't need to change.
 *
 * If a symbol fails to resolve, the function pointer stays NULL and
 * calling it will segfault. Callers should check idk_gl_loader_init()
 * return value before using GL.
 */
#ifndef IDK_GL_LOADER_H
#define IDK_GL_LOADER_H

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

/* GL enums used by compositor.c */
#define GL_NO_ERROR             0
#define GL_INVALID_ENUM         0x0500
#define GL_INVALID_VALUE        0x0501
#define GL_INVALID_OPERATION    0x0502
#define GL_INVALID_FRAMEBUFFER  0x0506
#define GL_OUT_OF_MEMORY        0x0505
#define GL_VIEWPORT             0x0BA2
#define GL_TEXTURE_2D           0x0DE1
#define GL_BLEND                0x0BE2
#define GL_BLEND_SRC_RGB        0x80C9
#define GL_BLEND_DST_RGB        0x80C8
#define GL_SRC_ALPHA            0x0302
#define GL_ONE_MINUS_SRC_ALPHA  0x0303
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_LINEAR               0x2601
#define GL_TRIANGLES            0x0004
#define GL_ARRAY_BUFFER         0x8892
#define GL_DYNAMIC_DRAW         0x88E8
#define GL_VERTEX_SHADER        0x8B31
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_INFO_LOG_LENGTH      0x8B84
#define GL_TEXTURE0             0x84C0
#define GL_FRAMEBUFFER              0x8D40
#define GL_DRAW_FRAMEBUFFER         0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER         0x8CA8
#define GL_FRAMEBUFFER_BINDING      0x8CA6
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_SAMPLER_BINDING      0x8919
#define GL_POLYGON_MODE         0x0B40
#define GL_FRONT_AND_BACK       0x0408
#define GL_FILL                 0x1B02
#define GL_PRIMITIVE_RESTART    0x8F9D
#define GL_DEPTH_WRITEMASK      0x0B72
#define GL_COLOR_WRITEMASK      0x0C23
#define GL_VERSION              0x1F02
#define GL_RENDERER             0x1F01

/* GL function pointers — X-macro pattern */

#define GL_FOREACH(F) \
    F(void,            glGetIntegerv,          (GLenum, GLint*)) \
    F(void,            glEnable,               (GLenum)) \
    F(void,            glDisable,              (GLenum)) \
    F(void,            glBlendFunc,            (GLenum, GLenum)) \
    F(GLuint,          glCreateShader,         (GLenum)) \
    F(void,            glShaderSource,         (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    F(void,            glCompileShader,        (GLuint)) \
    F(void,            glGetShaderiv,          (GLuint, GLenum, GLint*)) \
    F(void,            glGetShaderInfoLog,     (GLuint, GLsizei, GLsizei*, GLchar*)) \
    F(void,            glDeleteShader,         (GLuint)) \
    F(GLuint,          glCreateProgram,        (void)) \
    F(void,            glAttachShader,         (GLuint, GLuint)) \
    F(void,            glLinkProgram,          (GLuint)) \
    F(void,            glGetProgramiv,         (GLuint, GLenum, GLint*)) \
    F(void,            glGetProgramInfoLog,    (GLuint, GLsizei, GLsizei*, GLchar*)) \
    F(void,            glDeleteProgram,        (GLuint)) \
    F(void,            glUseProgram,           (GLuint)) \
    F(void,            glGenTextures,          (GLsizei, GLuint*)) \
    F(void,            glBindTexture,          (GLenum, GLuint)) \
    F(void,            glTexParameteri,        (GLenum, GLenum, GLint)) \
    F(void,            glDeleteTextures,       (GLsizei, const GLuint*)) \
    F(GLint,           glGetUniformLocation,   (GLuint, const GLchar*)) \
    F(void,            glUniform1i,            (GLint, GLint)) \
    F(void,            glActiveTexture,        (GLenum)) \
    F(void,            glGenBuffers,           (GLsizei, GLuint*)) \
    F(void,            glBindBuffer,           (GLenum, GLuint)) \
    F(void,            glBufferData,           (GLenum, GLsizei, const GLvoid*, GLenum)) \
    F(void,            glBufferSubData,        (GLenum, GLintptr, GLsizei, const GLvoid*)) \
    F(void,            glDeleteBuffers,        (GLsizei, const GLuint*)) \
    F(void,            glDrawArrays,           (GLenum, GLint, GLsizei)) \
    F(void,            glTexImage2D,           (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*)) \
    F(void,            glTexSubImage2D,        (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid*)) \
    F(void,            glPixelStorei,          (GLenum, GLint)) \
    F(void,            glEnableVertexAttribArray, (GLuint)) \
    F(void,            glDisableVertexAttribArray, (GLuint)) \
    F(void,            glVertexAttribPointer,  (GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*)) \
    F(GLint,           glGetAttribLocation,    (GLuint, const GLchar*)) \
    F(GLboolean,       glIsEnabled,            (GLenum)) \
    F(GLboolean,       glIsTexture,            (GLuint)) \
    F(GLboolean,       glIsProgram,            (GLuint)) \
    F(GLenum,          glGetError,             (void)) \
    F(void,            glFinish,               (void)) \
    F(void,            glClear,                (GLbitfield)) \
    F(void,            glClearColor,           (GLfloat, GLfloat, GLfloat, GLfloat)) \
    F(void,            glDrawBuffer,           (GLenum)) \
    F(void,            glBlendFuncSeparate,    (GLenum, GLenum, GLenum, GLenum)) \
    F(void,            glBlendEquation,        (GLenum)) \
    F(void,            glBlendEquationSeparate,(GLenum, GLenum)) \
    F(void,            glScissor,              (GLint, GLint, GLsizei, GLsizei)) \
    F(void,            glViewport,             (GLint, GLint, GLsizei, GLsizei)) \
    F(const GLubyte*,  glGetString,            (GLenum)) \
    F(void,            glDepthMask,            (GLboolean)) \
    F(void,            glColorMask,            (GLboolean, GLboolean, GLboolean, GLboolean)) \
    F(void,            glGetVertexAttribiv,    (GLuint, GLenum, GLint*)) \
    F(void,            glGenVertexArrays,      (GLsizei, GLuint*)) \
    F(void,            glBindVertexArray,      (GLuint)) \
    F(void,            glDeleteVertexArrays,   (GLsizei, const GLuint*)) \
    F(void,            glBindSampler,          (GLuint, GLuint)) \
    F(void,            glPolygonMode,          (GLenum, GLenum)) \
    F(void,            glBindFramebuffer,      (GLenum, GLuint)) \
    F(void,            glShaderBinary,         (GLsizei, const GLuint*, GLenum, const GLvoid*, GLsizei)) \
    F(void,            glSpecializeShader,     (GLuint, const GLchar*, GLuint, const GLuint*, const GLuint*))

#define GL_TYPEDEF(ret, name, params) typedef ret (*PFN_idk_##name) params;
GL_FOREACH(GL_TYPEDEF)
#undef GL_TYPEDEF

#define GL_EXTERN(ret, name, params) extern PFN_idk_##name idk_fn_##name;
GL_FOREACH(GL_EXTERN)
#undef GL_EXTERN

/* Macro redirect: glGetIntegerv → (*idk_fn_glGetIntegerv)
 *
 * This lets compositor.c call glGetIntegerv(...) as if it were a normal
 * function. The macro rewrites to (*idk_fn_glGetIntegerv)(...) which
 * dereferences the function pointer.
 *
 * IMPORTANT: this header must be included AFTER any system GL headers
 * (so the macro doesn't conflict with real declarations). In practice
 * we just don't include system GL headers at all. */

#define glGetIntegerv          (*idk_fn_glGetIntegerv)
#define glEnable               (*idk_fn_glEnable)
#define glDisable              (*idk_fn_glDisable)
#define glBlendFunc            (*idk_fn_glBlendFunc)
#define glCreateShader         (*idk_fn_glCreateShader)
#define glShaderSource         (*idk_fn_glShaderSource)
#define glCompileShader        (*idk_fn_glCompileShader)
#define glGetShaderiv          (*idk_fn_glGetShaderiv)
#define glGetShaderInfoLog     (*idk_fn_glGetShaderInfoLog)
#define glDeleteShader         (*idk_fn_glDeleteShader)
#define glCreateProgram        (*idk_fn_glCreateProgram)
#define glAttachShader         (*idk_fn_glAttachShader)
#define glLinkProgram          (*idk_fn_glLinkProgram)
#define glGetProgramiv         (*idk_fn_glGetProgramiv)
#define glGetProgramInfoLog    (*idk_fn_glGetProgramInfoLog)
#define glDeleteProgram        (*idk_fn_glDeleteProgram)
#define glUseProgram           (*idk_fn_glUseProgram)
#define glGenTextures          (*idk_fn_glGenTextures)
#define glBindTexture          (*idk_fn_glBindTexture)
#define glTexParameteri        (*idk_fn_glTexParameteri)
#define glDeleteTextures       (*idk_fn_glDeleteTextures)
#define glGetUniformLocation  (*idk_fn_glGetUniformLocation)
#define glUniform1i            (*idk_fn_glUniform1i)
#define glActiveTexture        (*idk_fn_glActiveTexture)
#define glGenBuffers           (*idk_fn_glGenBuffers)
#define glBindBuffer           (*idk_fn_glBindBuffer)
#define glBufferData           (*idk_fn_glBufferData)
#define glBufferSubData        (*idk_fn_glBufferSubData)
#define glDeleteBuffers        (*idk_fn_glDeleteBuffers)
#define glDrawArrays           (*idk_fn_glDrawArrays)
#define glTexImage2D           (*idk_fn_glTexImage2D)
#define glTexSubImage2D        (*idk_fn_glTexSubImage2D)
#define glPixelStorei          (*idk_fn_glPixelStorei)
#define glEnableVertexAttribArray  (*idk_fn_glEnableVertexAttribArray)
#define glDisableVertexAttribArray (*idk_fn_glDisableVertexAttribArray)
#define glVertexAttribPointer  (*idk_fn_glVertexAttribPointer)
#define glGetAttribLocation    (*idk_fn_glGetAttribLocation)
#define glIsEnabled            (*idk_fn_glIsEnabled)
#define glIsTexture            (*idk_fn_glIsTexture)
#define glIsProgram            (*idk_fn_glIsProgram)
#define glGetError             (*idk_fn_glGetError)
#define glFinish               (*idk_fn_glFinish)
#define glClear                (*idk_fn_glClear)
#define glClearColor           (*idk_fn_glClearColor)
#define glDrawBuffer           (*idk_fn_glDrawBuffer)
#define glBlendFuncSeparate    (*idk_fn_glBlendFuncSeparate)
#define glBlendEquation        (*idk_fn_glBlendEquation)
#define glBlendEquationSeparate (*idk_fn_glBlendEquationSeparate)
#define glScissor              (*idk_fn_glScissor)
#define glViewport             (*idk_fn_glViewport)
#define glGetString            (*idk_fn_glGetString)
#define glDepthMask            (*idk_fn_glDepthMask)
#define glColorMask            (*idk_fn_glColorMask)
#define glGetVertexAttribiv    (*idk_fn_glGetVertexAttribiv)
#define glGenVertexArrays      (*idk_fn_glGenVertexArrays)
#define glBindVertexArray      (*idk_fn_glBindVertexArray)
#define glDeleteVertexArrays   (*idk_fn_glDeleteVertexArrays)
#define glBindSampler          (*idk_fn_glBindSampler)
#define glPolygonMode          (*idk_fn_glPolygonMode)
#define glBindFramebuffer      (*idk_fn_glBindFramebuffer)
#define glShaderBinary         (*idk_fn_glShaderBinary)
#define glSpecializeShader     (*idk_fn_glSpecializeShader)

/* GL draw buffer and clear enums */
#define GL_DRAW_BUFFER          0x0C01
#define GL_BACK                 0x0405
#define GL_NONE                 0
#define GL_COLOR_BUFFER_BIT     0x00004000

/* Additional GL enums for SHM texture upload + render */
#define GL_RGBA                0x1908
#define GL_RGBA8               0x8058
#define GL_BGRA                0x80E1
#define GL_UNSIGNED_BYTE       0x1401
#define GL_UNPACK_ROW_LENGTH   0x0CF2
#define GL_UNPACK_ALIGNMENT    0x0CF5
#define GL_CURRENT_PROGRAM     0x8B8D
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_TEXTURE_BINDING_2D  0x8069
#define GL_FLOAT               0x1406
#define GL_FALSE               0
#define GL_ACTIVE_TEXTURE      0x84E0
#define GL_BLEND_SRC_ALPHA     0x80CB
#define GL_BLEND_DST_ALPHA     0x80CA
#define GL_DEPTH_TEST          0x0B71
#define GL_CULL_FACE           0x0B44
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define GL_TRUE                1
#define GL_BLEND_EQUATION_RGB  0x8009
#define GL_BLEND_EQUATION_ALPHA 0x883D
#define GL_FUNC_ADD            0x8006
#define GL_SCISSOR_TEST        0x0C11
#define GL_SCISSOR_BOX         0x0C10
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_FRAMEBUFFER_SRGB    0x8DB9
#define GL_STENCIL_TEST         0x0B90
#define GL_ONE                  1

/* SPIR-V enums */
#define GL_SHADER_BINARY_FORMAT_SPIR_V   0x9307
#define GL_SHADER_BINARY_FORMATS         0x8207

/* Init: resolve GL function pointers via dlsym. Tries libGL.so.1 first,
 * then libGLESv2.so.2, then libGL.so / libGLESv2.so. Uses the already-
 * loaded library (RTLD_NOLOAD) if available.
 * @return 0 on success, -1 on failure. Individual function pointers may
 *         be NULL even on success - caller should check critical ones. */
int idk_gl_loader_init(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_GL_LOADER_H */
