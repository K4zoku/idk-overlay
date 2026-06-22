#pragma once

#include <stddef.h>

/* ── GLSL embedded shaders (ld -b binary + objcopy rename) ──────────── */
/* Body stripped of #version line via sed at build time.                 */
/* compositor.c prepends the correct #version at runtime.                */

#define GLSL_SHADER(name) \
    extern const char glsl_overlay_##name[]; \
    extern const size_t glsl_overlay_##name##_size

GLSL_SHADER(vertex_120);
GLSL_SHADER(fragment_120);
GLSL_SHADER(vertex_130);
GLSL_SHADER(fragment_130);
GLSL_SHADER(vertex_300_es);
GLSL_SHADER(fragment_300_es);
GLSL_SHADER(vertex_410);
GLSL_SHADER(fragment_410);

#undef GLSL_SHADER

/* ── SPIR-V embedded shaders (glslc + ld -b binary + objcopy rename) ── */
/* Only available when HAS_SPIRV is defined at build time.                */

#ifdef HAS_SPIRV

#define SPV_SHADER(name) \
    extern const unsigned char spv_##name[]; \
    extern const size_t spv_##name##_size

SPV_SHADER(vertex_120);
SPV_SHADER(fragment_120);
SPV_SHADER(vertex_130);
SPV_SHADER(fragment_130);
SPV_SHADER(vertex_300_es);
SPV_SHADER(fragment_300_es);
SPV_SHADER(vertex_410);
SPV_SHADER(fragment_410);

#undef SPV_SHADER

#endif /* HAS_SPIRV */
