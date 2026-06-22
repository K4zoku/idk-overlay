/*
 * idk_gl_loader.h — Runtime GL symbol resolution
 *
 * Pattern: all GL functions are resolved at runtime via dlsym, never
 * linked at build time. This lets libidk-overlay.so inject into any
 * process regardless of which GL implementation it uses (Mesa GL,
 * Mesa GLES, NVIDIA, etc.) — we just grab whatever is already loaded.
 *
 * Usage:
 *   1. #include "idk_gl_loader.h" in your .c file
 *   2. Call idk_gl_loader_init() once, from a GL context
 *   3. Use GL functions as normal (macros redirect to function pointers)
 *
 * The macros below redirect direct calls like glGetIntegerv(...) to
 * (*idk_fn_glGetIntegerv)(...) — so existing code that calls GL functions
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

/* ── GL types (subset, enough for compositor.c) ────────────────────────── */

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

/* ── Function pointer typedefs ──────────────────────────────────────────── */

typedef void (*PFN_idk_glGetIntegerv)(GLenum, GLint*);
typedef void (*PFN_idk_glEnable)(GLenum);
typedef void (*PFN_idk_glDisable)(GLenum);
typedef void (*PFN_idk_glBlendFunc)(GLenum, GLenum);
typedef GLuint (*PFN_idk_glCreateShader)(GLenum);
typedef void (*PFN_idk_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void (*PFN_idk_glCompileShader)(GLuint);
typedef void (*PFN_idk_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void (*PFN_idk_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void (*PFN_idk_glDeleteShader)(GLuint);
typedef GLuint (*PFN_idk_glCreateProgram)(void);
typedef void (*PFN_idk_glAttachShader)(GLuint, GLuint);
typedef void (*PFN_idk_glLinkProgram)(GLuint);
typedef void (*PFN_idk_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void (*PFN_idk_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void (*PFN_idk_glDeleteProgram)(GLuint);
typedef void (*PFN_idk_glUseProgram)(GLuint);
typedef void (*PFN_idk_glGenTextures)(GLsizei, GLuint*);
typedef void (*PFN_idk_glBindTexture)(GLenum, GLuint);
typedef void (*PFN_idk_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (*PFN_idk_glDeleteTextures)(GLsizei, const GLuint*);
typedef GLint (*PFN_idk_glGetUniformLocation)(GLuint, const GLchar*);
typedef void (*PFN_idk_glUniform1i)(GLint, GLint);
typedef void (*PFN_idk_glActiveTexture)(GLenum);
typedef void (*PFN_idk_glGenBuffers)(GLsizei, GLuint*);
typedef void (*PFN_idk_glBindBuffer)(GLenum, GLuint);
typedef void (*PFN_idk_glBufferData)(GLenum, GLsizei, const GLvoid*, GLenum);
typedef void (*PFN_idk_glBufferSubData)(GLenum, GLintptr, GLsizei, const GLvoid*);
typedef void (*PFN_idk_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void (*PFN_idk_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (*PFN_idk_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
typedef void (*PFN_idk_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid*);
typedef void (*PFN_idk_glPixelStorei)(GLenum, GLint);
typedef void (*PFN_idk_glEnableVertexAttribArray)(GLuint);
typedef void (*PFN_idk_glDisableVertexAttribArray)(GLuint);
typedef void (*PFN_idk_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*);
typedef GLint (*PFN_idk_glGetAttribLocation)(GLuint, const GLchar*);
typedef GLboolean (*PFN_idk_glIsEnabled)(GLenum);
typedef GLboolean (*PFN_idk_glIsTexture)(GLuint);
typedef GLboolean (*PFN_idk_glIsProgram)(GLuint);
typedef GLenum (*PFN_idk_glGetError)(void);
typedef void (*PFN_idk_glFinish)(void);
typedef void (*PFN_idk_glClear)(GLbitfield);
typedef void (*PFN_idk_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_idk_glDrawBuffer)(GLenum);
typedef void (*PFN_idk_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void (*PFN_idk_glBlendEquation)(GLenum);
typedef void (*PFN_idk_glBlendEquationSeparate)(GLenum, GLenum);
typedef void (*PFN_idk_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_idk_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef const GLubyte* (*PFN_idk_glGetString)(GLenum);
typedef void (*PFN_idk_glDepthMask)(GLboolean);
typedef void (*PFN_idk_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (*PFN_idk_glGetVertexAttribiv)(GLuint, GLenum, GLint*);
typedef void (*PFN_idk_glGenVertexArrays)(GLsizei, GLuint*);
typedef void (*PFN_idk_glBindVertexArray)(GLuint);
typedef void (*PFN_idk_glDeleteVertexArrays)(GLsizei, const GLuint*);
typedef void (*PFN_idk_glBindSampler)(GLuint, GLuint);
typedef void (*PFN_idk_glPolygonMode)(GLenum, GLenum);
typedef void (*PFN_idk_glBindFramebuffer)(GLenum, GLuint);

/* ── SPIR-V function pointer typedefs ────────────────────────────────── */
typedef void (*PFN_idk_glShaderBinary)(GLsizei, const GLuint*, GLenum, const GLvoid*, GLsizei);
typedef void (*PFN_idk_glSpecializeShader)(GLuint, const GLchar*, GLuint, const GLuint*, const GLuint*);

/* ── Global function pointers (resolved by idk_gl_loader_init) ──────────── */

extern PFN_idk_glGetIntegerv          idk_fn_glGetIntegerv;
extern PFN_idk_glEnable               idk_fn_glEnable;
extern PFN_idk_glDisable              idk_fn_glDisable;
extern PFN_idk_glBlendFunc            idk_fn_glBlendFunc;
extern PFN_idk_glCreateShader         idk_fn_glCreateShader;
extern PFN_idk_glShaderSource         idk_fn_glShaderSource;
extern PFN_idk_glCompileShader        idk_fn_glCompileShader;
extern PFN_idk_glGetShaderiv          idk_fn_glGetShaderiv;
extern PFN_idk_glGetShaderInfoLog     idk_fn_glGetShaderInfoLog;
extern PFN_idk_glDeleteShader         idk_fn_glDeleteShader;
extern PFN_idk_glCreateProgram        idk_fn_glCreateProgram;
extern PFN_idk_glAttachShader         idk_fn_glAttachShader;
extern PFN_idk_glLinkProgram          idk_fn_glLinkProgram;
extern PFN_idk_glGetProgramiv         idk_fn_glGetProgramiv;
extern PFN_idk_glGetProgramInfoLog    idk_fn_glGetProgramInfoLog;
extern PFN_idk_glDeleteProgram        idk_fn_glDeleteProgram;
extern PFN_idk_glUseProgram           idk_fn_glUseProgram;
extern PFN_idk_glGenTextures          idk_fn_glGenTextures;
extern PFN_idk_glBindTexture          idk_fn_glBindTexture;
extern PFN_idk_glTexParameteri        idk_fn_glTexParameteri;
extern PFN_idk_glDeleteTextures       idk_fn_glDeleteTextures;
extern PFN_idk_glGetUniformLocation   idk_fn_glGetUniformLocation;
extern PFN_idk_glUniform1i            idk_fn_glUniform1i;
extern PFN_idk_glActiveTexture        idk_fn_glActiveTexture;
extern PFN_idk_glGenBuffers           idk_fn_glGenBuffers;
extern PFN_idk_glBindBuffer           idk_fn_glBindBuffer;
extern PFN_idk_glBufferData           idk_fn_glBufferData;
extern PFN_idk_glBufferSubData        idk_fn_glBufferSubData;
extern PFN_idk_glDeleteBuffers        idk_fn_glDeleteBuffers;
extern PFN_idk_glDrawArrays           idk_fn_glDrawArrays;
extern PFN_idk_glTexImage2D           idk_fn_glTexImage2D;
extern PFN_idk_glTexSubImage2D        idk_fn_glTexSubImage2D;
extern PFN_idk_glPixelStorei          idk_fn_glPixelStorei;
extern PFN_idk_glEnableVertexAttribArray idk_fn_glEnableVertexAttribArray;
extern PFN_idk_glDisableVertexAttribArray idk_fn_glDisableVertexAttribArray;
extern PFN_idk_glVertexAttribPointer  idk_fn_glVertexAttribPointer;
extern PFN_idk_glGetAttribLocation    idk_fn_glGetAttribLocation;
extern PFN_idk_glIsEnabled            idk_fn_glIsEnabled;
extern PFN_idk_glIsTexture            idk_fn_glIsTexture;
extern PFN_idk_glIsProgram            idk_fn_glIsProgram;
extern PFN_idk_glGetError            idk_fn_glGetError;
extern PFN_idk_glFinish             idk_fn_glFinish;
extern PFN_idk_glClear              idk_fn_glClear;
extern PFN_idk_glClearColor         idk_fn_glClearColor;
extern PFN_idk_glDrawBuffer         idk_fn_glDrawBuffer;
extern PFN_idk_glBlendFuncSeparate    idk_fn_glBlendFuncSeparate;
extern PFN_idk_glBlendEquation        idk_fn_glBlendEquation;
extern PFN_idk_glBlendEquationSeparate idk_fn_glBlendEquationSeparate;
extern PFN_idk_glScissor              idk_fn_glScissor;
extern PFN_idk_glViewport             idk_fn_glViewport;
extern PFN_idk_glGetString            idk_fn_glGetString;
extern PFN_idk_glDepthMask            idk_fn_glDepthMask;
extern PFN_idk_glColorMask            idk_fn_glColorMask;
extern PFN_idk_glGetVertexAttribiv    idk_fn_glGetVertexAttribiv;
extern PFN_idk_glGenVertexArrays      idk_fn_glGenVertexArrays;
extern PFN_idk_glBindVertexArray      idk_fn_glBindVertexArray;
extern PFN_idk_glDeleteVertexArrays   idk_fn_glDeleteVertexArrays;
extern PFN_idk_glBindSampler          idk_fn_glBindSampler;
extern PFN_idk_glPolygonMode          idk_fn_glPolygonMode;
extern PFN_idk_glBindFramebuffer      idk_fn_glBindFramebuffer;
extern PFN_idk_glShaderBinary         idk_fn_glShaderBinary;
extern PFN_idk_glSpecializeShader     idk_fn_glSpecializeShader;

/* ── Macro redirect: glGetIntegerv → (*idk_fn_glGetIntegerv) ──────────────
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

/* ── SPIR-V enums ─────────────────────────────────────────────────────── */
#define GL_SHADER_BINARY_FORMAT_SPIR_V   0x9307
#define GL_SHADER_BINARY_FORMATS         0x8207

/* ── Init ─────────────────────────────────────────────────────────────────
 *
 * Resolves all GL function pointers via dlsym. Tries libGL.so.1 first,
 * then libGLESv2.so.2, then libGL.so / libGLESv2.so. Uses RTLD_NOLOAD
 * to grab the already-loaded library if the target process has one.
 *
 * @return 0 on success, -1 on failure (no GL library found).
 *         Individual function pointers may be NULL even on success —
 *         caller should check critical ones before use. */
int idk_gl_loader_init(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_GL_LOADER_H */
