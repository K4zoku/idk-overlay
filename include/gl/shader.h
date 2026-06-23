#pragma once

#include <stddef.h>
#include <stdint.h>

/* ── Helpers ────────────────────────────────────────────────────────── */
/* #version line is stripped at build time via sed; compositor prepends
 * the correct #version at runtime based on GL version detection.      */

/*
 * IMPORTANT: the _size symbols are ABSOLUTE symbols (type A in nm output),
 * created by `ld -r -b binary`. Their VALUE is the byte count — they are
 * NOT pointers to memory containing the size.
 *
 * Declaring them as `extern const size_t sym_size` and then reading
 * `sym_size` generates a LOAD instruction from address = symbol value
 * (e.g. 0x242 for a 578-byte shader). That address is unmapped → SEGFAULT.
 *
 * Fix: declare as `extern const char sym_size[]` (array, not scalar).
 * Using the array name in an expression decays to a pointer whose value
 * IS the absolute symbol value (= the byte count). Casting that pointer
 * to size_t via the _SIZE macros below gives the correct size without
 * any memory load.
 */

#define GLSL_SHADER(name) \
    extern const char glsl_overlay_##name[]; \
    extern const char glsl_overlay_##name##_size[]

#define GLSL_SHADER_SIZE(name) ((size_t)(uintptr_t)glsl_overlay_##name##_size)

#define GLSL_SHADER_PAIR(ver) \
    GLSL_SHADER(vertex_##ver); \
    GLSL_SHADER(fragment_##ver)

/* ── GLSL embedded shaders (always available ─────────────────────────── */

GLSL_SHADER_PAIR(120);
GLSL_SHADER_PAIR(130);
GLSL_SHADER_PAIR(300_es);
GLSL_SHADER_PAIR(410);

#undef GLSL_SHADER
#undef GLSL_SHADER_PAIR

/* ── SPIR-V embedded shaders (per-variant, build-time only) ─────────── */
/* Only defined when the variant's glslc compile succeeded at build time.
 * Each variant has its own HAS_SPV_* define — missing ones = SPIR-V
 * couldn't compile that flavour.                                        */

#ifdef __cplusplus
extern "C" {
#endif

#define SPV_SHADER(name) \
    extern const unsigned char spv_##name[]; \
    extern const unsigned char spv_##name##_size[]

#define SPV_SHADER_SIZE(name) ((size_t)(uintptr_t)spv_##name##_size)

#define SPV_SHADER_PAIR(ver) \
    SPV_SHADER(vertex_##ver); \
    SPV_SHADER(fragment_##ver)

#ifdef HAS_SPV_120
SPV_SHADER_PAIR(120);
#endif
#ifdef HAS_SPV_130
SPV_SHADER_PAIR(130);
#endif
#ifdef HAS_SPV_300_es
SPV_SHADER_PAIR(300_es);
#endif
#ifdef HAS_SPV_410
SPV_SHADER_PAIR(410);
#endif

#undef SPV_SHADER
#undef SPV_SHADER_PAIR

#ifdef __cplusplus
}
#endif
