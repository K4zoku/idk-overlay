/*
 * gl_loader.c — Runtime GL symbol resolution
 *
 * Resolves all GL function pointers via dlsym at runtime. No link-time
 * dependency on libGL / libGLESv2 — we grab whatever the target process
 * has already loaded (RTLD_NOLOAD).
 *
 * The compositor (compositor.c) calls idk_gl_loader_init() once when
 * it first has a GL context, then uses GL functions normally — the
 * macros in idk_gl_loader.h redirect calls to the function pointers
 * resolved here.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include "public/idk_gl_loader.h"
#include "public/idk_log.h"

/* ── Global function pointers (default NULL) ────────────────────────────── */

PFN_idk_glGetIntegerv          idk_fn_glGetIntegerv          = NULL;
PFN_idk_glEnable               idk_fn_glEnable               = NULL;
PFN_idk_glDisable              idk_fn_glDisable              = NULL;
PFN_idk_glBlendFunc            idk_fn_glBlendFunc            = NULL;
PFN_idk_glCreateShader         idk_fn_glCreateShader         = NULL;
PFN_idk_glShaderSource         idk_fn_glShaderSource         = NULL;
PFN_idk_glCompileShader        idk_fn_glCompileShader        = NULL;
PFN_idk_glGetShaderiv          idk_fn_glGetShaderiv          = NULL;
PFN_idk_glGetShaderInfoLog     idk_fn_glGetShaderInfoLog     = NULL;
PFN_idk_glDeleteShader         idk_fn_glDeleteShader         = NULL;
PFN_idk_glCreateProgram        idk_fn_glCreateProgram        = NULL;
PFN_idk_glAttachShader         idk_fn_glAttachShader         = NULL;
PFN_idk_glLinkProgram          idk_fn_glLinkProgram          = NULL;
PFN_idk_glGetProgramiv         idk_fn_glGetProgramiv         = NULL;
PFN_idk_glGetProgramInfoLog    idk_fn_glGetProgramInfoLog    = NULL;
PFN_idk_glDeleteProgram        idk_fn_glDeleteProgram        = NULL;
PFN_idk_glUseProgram           idk_fn_glUseProgram           = NULL;
PFN_idk_glGenTextures          idk_fn_glGenTextures          = NULL;
PFN_idk_glBindTexture          idk_fn_glBindTexture          = NULL;
PFN_idk_glTexParameteri        idk_fn_glTexParameteri        = NULL;
PFN_idk_glDeleteTextures       idk_fn_glDeleteTextures       = NULL;
PFN_idk_glGetUniformLocation   idk_fn_glGetUniformLocation   = NULL;
PFN_idk_glUniform1i            idk_fn_glUniform1i            = NULL;
PFN_idk_glActiveTexture        idk_fn_glActiveTexture        = NULL;
PFN_idk_glGenBuffers           idk_fn_glGenBuffers           = NULL;
PFN_idk_glBindBuffer           idk_fn_glBindBuffer           = NULL;
PFN_idk_glBufferData           idk_fn_glBufferData           = NULL;
PFN_idk_glBufferSubData        idk_fn_glBufferSubData        = NULL;
PFN_idk_glDeleteBuffers        idk_fn_glDeleteBuffers        = NULL;
PFN_idk_glDrawArrays           idk_fn_glDrawArrays           = NULL;
PFN_idk_glTexImage2D           idk_fn_glTexImage2D           = NULL;
PFN_idk_glTexSubImage2D        idk_fn_glTexSubImage2D        = NULL;
PFN_idk_glPixelStorei          idk_fn_glPixelStorei          = NULL;
PFN_idk_glEnableVertexAttribArray idk_fn_glEnableVertexAttribArray = NULL;
PFN_idk_glDisableVertexAttribArray idk_fn_glDisableVertexAttribArray = NULL;
PFN_idk_glVertexAttribPointer  idk_fn_glVertexAttribPointer  = NULL;
PFN_idk_glGetAttribLocation    idk_fn_glGetAttribLocation    = NULL;
PFN_idk_glIsEnabled            idk_fn_glIsEnabled            = NULL;
PFN_idk_glIsTexture            idk_fn_glIsTexture            = NULL;
PFN_idk_glIsProgram            idk_fn_glIsProgram            = NULL;
PFN_idk_glGetError            idk_fn_glGetError            = NULL;
PFN_idk_glFinish               idk_fn_glFinish               = NULL;
PFN_idk_glClear                idk_fn_glClear                = NULL;
PFN_idk_glClearColor           idk_fn_glClearColor           = NULL;
PFN_idk_glDrawBuffer           idk_fn_glDrawBuffer           = NULL;
PFN_idk_glBlendFuncSeparate    idk_fn_glBlendFuncSeparate    = NULL;
PFN_idk_glBlendEquation        idk_fn_glBlendEquation        = NULL;
PFN_idk_glBlendEquationSeparate idk_fn_glBlendEquationSeparate = NULL;
PFN_idk_glScissor              idk_fn_glScissor              = NULL;
PFN_idk_glViewport             idk_fn_glViewport             = NULL;
PFN_idk_glGetString            idk_fn_glGetString            = NULL;
PFN_idk_glDepthMask            idk_fn_glDepthMask            = NULL;
PFN_idk_glColorMask            idk_fn_glColorMask            = NULL;
PFN_idk_glGetVertexAttribiv    idk_fn_glGetVertexAttribiv    = NULL;
PFN_idk_glGenVertexArrays      idk_fn_glGenVertexArrays      = NULL;
PFN_idk_glBindVertexArray      idk_fn_glBindVertexArray      = NULL;
PFN_idk_glDeleteVertexArrays   idk_fn_glDeleteVertexArrays   = NULL;
PFN_idk_glBindSampler          idk_fn_glBindSampler          = NULL;
PFN_idk_glPolygonMode          idk_fn_glPolygonMode          = NULL;
PFN_idk_glBindFramebuffer      idk_fn_glBindFramebuffer      = NULL;

/* ── Init ──────────────────────────────────────────────────────────────────
 *
 * Try in order:
 *   1. libGL.so.1 (desktop OpenGL, most common)
 *   2. libGLESv2.so.2 (embedded / Wayland EGL apps)
 *   3. libGL.so (unversioned fallback)
 *   4. libGLESv2.so (unversioned fallback)
 *
 * RTLD_NOLOAD first — if target already loaded one of these, reuse it.
 * This is important: the GL context the target created is bound to the
 * specific libGL.so.1 it loaded. We must use the SAME library.
 */
int idk_gl_loader_init(void) {
    static int tried = 0;
    static int result = -1;
    if (tried) return result;
    tried = 1;

    void *lib = NULL;
    const char *lib_names[] = {
        "libGL.so.1",
        "libGLESv2.so.2",
        "libGL.so",
        "libGLESv2.so",
    };

    for (size_t i = 0; i < sizeof(lib_names)/sizeof(*lib_names); i++) {
        lib = dlopen(lib_names[i], RTLD_NOW | RTLD_NOLOAD);
        if (!lib) {
            lib = dlopen(lib_names[i], RTLD_NOW | RTLD_GLOBAL);
        }
        if (lib) {
            IDK_LOG("gl-loader", "Using %s\n", lib_names[i]);
            break;
        }
    }
    if (!lib) {
        IDK_ERR("gl-loader", "No GL library found\n");
        return -1;
    }

    /* Resolve all symbols. Don't bail on first NULL — some functions
     * may be missing (e.g. GLES2 doesn't have everything desktop GL has).
     * Caller checks critical ones. */
    #define RESOLVE(name) \
        idk_fn_##name = (PFN_idk_##name)dlsym(lib, #name)

    RESOLVE(glGetIntegerv);
    RESOLVE(glEnable);
    RESOLVE(glDisable);
    RESOLVE(glBlendFunc);
    RESOLVE(glCreateShader);
    RESOLVE(glShaderSource);
    RESOLVE(glCompileShader);
    RESOLVE(glGetShaderiv);
    RESOLVE(glGetShaderInfoLog);
    RESOLVE(glDeleteShader);
    RESOLVE(glCreateProgram);
    RESOLVE(glAttachShader);
    RESOLVE(glLinkProgram);
    RESOLVE(glGetProgramiv);
    RESOLVE(glGetProgramInfoLog);
    RESOLVE(glDeleteProgram);
    RESOLVE(glUseProgram);
    RESOLVE(glGenTextures);
    RESOLVE(glBindTexture);
    RESOLVE(glTexParameteri);
    RESOLVE(glDeleteTextures);
    RESOLVE(glGetUniformLocation);
    RESOLVE(glUniform1i);
    RESOLVE(glActiveTexture);
    RESOLVE(glGenBuffers);
    RESOLVE(glBindBuffer);
    RESOLVE(glBufferData);
    RESOLVE(glBufferSubData);
    RESOLVE(glDeleteBuffers);
    RESOLVE(glDrawArrays);
    RESOLVE(glTexImage2D);
    RESOLVE(glTexSubImage2D);
    RESOLVE(glPixelStorei);
    RESOLVE(glEnableVertexAttribArray);
    RESOLVE(glDisableVertexAttribArray);
    RESOLVE(glVertexAttribPointer);
    RESOLVE(glGetAttribLocation);
    RESOLVE(glIsEnabled);
    RESOLVE(glIsTexture);
    RESOLVE(glIsProgram);
    RESOLVE(glGetError);
    RESOLVE(glFinish);
    RESOLVE(glClear);
    RESOLVE(glClearColor);
    RESOLVE(glDrawBuffer);
    RESOLVE(glBlendFuncSeparate);
    RESOLVE(glBlendEquation);
    RESOLVE(glBlendEquationSeparate);
    RESOLVE(glScissor);
    RESOLVE(glViewport);
    RESOLVE(glGetString);
    RESOLVE(glDepthMask);
    RESOLVE(glColorMask);
    RESOLVE(glGetVertexAttribiv);
    RESOLVE(glGenVertexArrays);
    RESOLVE(glBindVertexArray);
    RESOLVE(glDeleteVertexArrays);
    RESOLVE(glBindSampler);
    RESOLVE(glPolygonMode);
    RESOLVE(glBindFramebuffer);

    #undef RESOLVE

    /* Sanity check: a few critical ones must be present */
    if (!idk_fn_glGetIntegerv || !idk_fn_glDrawArrays || !idk_fn_glUseProgram) {
        IDK_ERR("gl-loader", "Critical GL functions missing — "
                        "glGetIntegerv=%p glDrawArrays=%p glUseProgram=%p\n",
                (void*)idk_fn_glGetIntegerv,
                (void*)idk_fn_glDrawArrays,
                (void*)idk_fn_glUseProgram);
        return -1;
    }

    IDK_LOG("gl-loader", "GL functions resolved\n");
    result = 0;
    return 0;
}
