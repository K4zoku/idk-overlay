/*
 * gl_loader.c - Runtime GL symbol resolution
 *
 * Resolves all GL function pointers via dlsym at runtime. No link-time
 * dependency on libGL / libGLESv2 - we grab whatever the target process
 * has already loaded (RTLD_NOLOAD).
 *
 * The compositor (compositor.c) calls idk_gl_loader_init() once when
 * it first has a GL context, then uses GL functions normally - the
 * macros in idk_gl_loader.h redirect calls to the function pointers
 * resolved here.
 */

#include <stdio.h>
#include <dlfcn.h>
#include "gl/gl_loader.h"
#include "core/log.h"

/* Global function pointers */

#define GL_DEFINE(ret, name, params) PFN_idk_##name idk_fn_##name = NULL;
GL_FOREACH(GL_DEFINE)
#undef GL_DEFINE

/* Init: try in order: libGL.so.1, libGLESv2.so.2, then unversioned.
 * Uses RTLD_NOLOAD first to grab already-loaded library.
 * RTLD_NOLOAD first - if target already loaded one of these, reuse it.
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

    /* Resolve all symbols via X-macro. Don't bail on first NULL - some
     * may be missing (e.g. GLES2 doesn't have everything desktop GL has).
     * Caller checks critical ones. */
#define GL_RESOLVE(ret, name, params) idk_fn_##name = (PFN_idk_##name)dlsym(lib, #name);
    GL_FOREACH(GL_RESOLVE)
#undef GL_RESOLVE

    if (!idk_fn_glGetIntegerv || !idk_fn_glDrawArrays || !idk_fn_glUseProgram) {
        IDK_ERR("gl-loader", "Critical GL functions missing - "
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
