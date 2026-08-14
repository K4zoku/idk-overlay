/* render/vk shaders.c - SPIR-V shader module creation (overlay_vk vert/frag) */

#include "context.h"
#include "core/log.h"
#include "shaders/vk_shaders.h"

/* Create the vertex + fragment shader modules from the embedded SPIR-V
 * bytecode (ld -b binary + objcopy; symbols declared in vk_shaders.h and
 * linked via spv_o_files in meson.build).
 * Vertex: fullscreen triangle (3 vertices, no VBO needed).
 * Fragment: sample texture, output premultiplied RGBA.
 * Returns 0 on success (caller destroys the modules), -1 otherwise. */
int vk_create_shader_modules(VkShaderModule *out_vert, VkShaderModule *out_frag) {
#ifdef HAS_VK_SPV
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkCreateShaderModule);
  VkShaderModuleCreateInfo vert_ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = VK_SPV_SHADER_SIZE(vert),
      .pCode = (const uint32_t *)spv_overlay_vk_vert,
  };
  VkShaderModule vert_mod;
  VkResult r = vkCreateShaderModule(ctx->device.vk_dev, &vert_ci, NULL, &vert_mod);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "vert shader module failed: %d\n", r);
    return -1;
  }

  VkShaderModuleCreateInfo frag_ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = VK_SPV_SHADER_SIZE(frag),
      .pCode = (const uint32_t *)spv_overlay_vk_frag,
  };
  VkShaderModule frag_mod;
  r = vkCreateShaderModule(ctx->device.vk_dev, &frag_ci, NULL, &frag_mod);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "frag shader module failed: %d\n", r);
    return -1;
  }

  *out_vert = vert_mod;
  *out_frag = frag_mod;
  return 0;
#else
  (void)out_vert;
  (void)out_frag;
  IDK_ERR("comp-vk", "VK SPIR-V shaders not built (glslc missing) - cannot create pipeline\n");
  return -1;
#endif
}
