#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Detect GL version from glGetString(GL_VERSION), select shader variant
 * matching that version, try SPIR-V first, fallback to GLSL, compile &
 * link. Returns the linked program handle.
 *
 * @param out_gl_version  [out] set to major*100 + minor*10 (e.g. 330)
 * @param out_is_gles     [out] set if the context is OpenGL ES
 * @return GLuint program handle, or 0 on failure
 *
 * Requires: idk_gl_loader_init() called first (GL function pointers
 * must be resolved). Must be called from a live GL context.
 */
unsigned int idk_shader_loader_init(int *out_gl_version, bool *out_is_gles);

/* SPIR-V support check (exposed for callers that want to log it) */
int idk_shader_loader_has_spirv(void);

#ifdef __cplusplus
}
#endif
