#pragma once

#include <stddef.h>
#include <stdint.h>

/* Vulkan overlay shader SPIR-V bytecode, embedded via ld -b binary + objcopy.
 * Same pattern as include/gl/shader.h for GL shaders.
 *
 * Symbols (produced by src/shaders/meson.build):
 *   spv_overlay_vk_vert, spv_overlay_vk_vert_size
 *   spv_overlay_vk_frag, spv_overlay_vk_frag_size
 *
 * IMPORTANT: the _size symbols are ABSOLUTE symbols (type A in nm output),
 * created by `ld -r -b binary`. Their VALUE is the byte count — they are
 * NOT pointers to memory containing the size. See include/gl/shader.h for
 * the full explanation of why these are declared as arrays and cast via
 * the _SIZE macros.
 *
 * Only available when HAS_VK_SPV is defined at build time (glslc found). */

#ifdef __cplusplus
extern "C" {
#endif

#define VK_SPV_SHADER(name) \
    extern const unsigned char spv_overlay_vk_##name[]; \
    extern const unsigned char spv_overlay_vk_##name##_size[]

#define VK_SPV_SHADER_SIZE(name) ((size_t)(uintptr_t)spv_overlay_vk_##name##_size)

#ifdef HAS_VK_SPV
VK_SPV_SHADER(vert);
VK_SPV_SHADER(frag);
#endif

#undef VK_SPV_SHADER

#ifdef __cplusplus
}
#endif
