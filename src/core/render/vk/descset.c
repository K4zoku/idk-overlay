/* render/vk descset.c - descriptor set layout, pipeline layout, sampler,
 * descriptor pool/set and command pool creation/destruction */

#include "context.h"
#include "core/log.h"

int vk_create_pipeline_objects(void) {
  vk_context_t *ctx = vk_ctx();
  VkResult r;

  VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  VkDescriptorSetLayoutCreateInfo dsl_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
  };
  VK_LOAD(vkCreateDescriptorSetLayout);
  r = vkCreateDescriptorSetLayout(ctx->device.vk_dev, &dsl_ci, NULL, &ctx->pipeline.vk_dsl);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "CreateDescriptorSetLayout failed: %d\n", r);
    return -1;
  }

  VkPipelineLayoutCreateInfo pll_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &ctx->pipeline.vk_dsl,
  };
  VK_LOAD(vkCreatePipelineLayout);
  r = vkCreatePipelineLayout(ctx->device.vk_dev, &pll_ci, NULL, &ctx->pipeline.vk_pll);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "CreatePipelineLayout failed: %d\n", r);
    return -1;
  }

  VkSamplerCreateInfo samp_ci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };
  VK_LOAD(vkCreateSampler);
  r = vkCreateSampler(ctx->device.vk_dev, &samp_ci, NULL, &ctx->pipeline.vk_sampler);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "CreateSampler failed: %d\n", r);
    return -1;
  }

  VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
  };
  VkDescriptorPoolCreateInfo pool_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
  };
  VK_LOAD(vkCreateDescriptorPool);
  r = vkCreateDescriptorPool(ctx->device.vk_dev, &pool_ci, NULL, &ctx->pipeline.vk_desc_pool);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "CreateDescriptorPool failed: %d\n", r);
    return -1;
  }

  VkDescriptorSetAllocateInfo ds_ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = ctx->pipeline.vk_desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &ctx->pipeline.vk_dsl,
  };
  VK_LOAD(vkAllocateDescriptorSets);
  r = vkAllocateDescriptorSets(ctx->device.vk_dev, &ds_ai, &ctx->pipeline.vk_desc_set);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "AllocateDescriptorSets failed: %d\n", r);
    return -1;
  }

  VkCommandPoolCreateInfo cmd_ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = ctx->device.vk_queue_family,
  };
  VK_LOAD(vkCreateCommandPool);
  r = vkCreateCommandPool(ctx->device.vk_dev, &cmd_ci, NULL, &ctx->device.vk_cmd_pool);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "CreateCommandPool failed: %d\n", r);
    return -1;
  }

  IDK_LOG("comp-vk", "Pipeline objects created\n");
  return 0;
}

/* Destroy pipeline + descriptor objects + command pool (shutdown).
 * Caller must have waited for the queue to idle first. */
void descset_destroy(void) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkDestroyPipeline);
  VK_LOAD(vkDestroyPipelineLayout);
  VK_LOAD(vkDestroyDescriptorSetLayout);
  VK_LOAD(vkDestroySampler);
  VK_LOAD(vkDestroyDescriptorPool);
  VK_LOAD(vkDestroyCommandPool);

  if (ctx->pipeline.vk_pipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(ctx->device.vk_dev, ctx->pipeline.vk_pipeline, NULL);
  ctx->pipeline.vk_pipeline = VK_NULL_HANDLE;
  if (ctx->pipeline.vk_pll != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(ctx->device.vk_dev, ctx->pipeline.vk_pll, NULL);
  ctx->pipeline.vk_pll = VK_NULL_HANDLE;
  if (ctx->pipeline.vk_dsl != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(ctx->device.vk_dev, ctx->pipeline.vk_dsl, NULL);
  ctx->pipeline.vk_dsl = VK_NULL_HANDLE;
  if (ctx->pipeline.vk_sampler != VK_NULL_HANDLE)
    vkDestroySampler(ctx->device.vk_dev, ctx->pipeline.vk_sampler, NULL);
  ctx->pipeline.vk_sampler = VK_NULL_HANDLE;
  if (ctx->pipeline.vk_desc_pool != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(ctx->device.vk_dev, ctx->pipeline.vk_desc_pool, NULL);
  ctx->pipeline.vk_desc_pool = VK_NULL_HANDLE;
  ctx->pipeline.vk_desc_set = VK_NULL_HANDLE;
  if (ctx->device.vk_cmd_pool != VK_NULL_HANDLE)
    vkDestroyCommandPool(ctx->device.vk_dev, ctx->device.vk_cmd_pool, NULL);
  ctx->device.vk_cmd_pool = VK_NULL_HANDLE;
}
