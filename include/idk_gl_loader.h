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
typedef void GLvoid;

/* GL enums used by compositor.c */
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
typedef void (*PFN_idk_glPixelStorei)(GLenum, GLint);

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
extern PFN_idk_glPixelStorei          idk_fn_glPixelStorei;

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
#define glPixelStorei          (*idk_fn_glPixelStorei)

/* Additional GL enums for SHM texture upload */
#define GL_RGBA                0x1908
#define GL_RGBA8               0x8058
#define GL_BGRA                0x80E1
#define GL_UNSIGNED_BYTE       0x1401
#define GL_UNPACK_ROW_LENGTH   0x0CF2
#define GL_UNPACK_ALIGNMENT    0x0CF5

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
