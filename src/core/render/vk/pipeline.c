/* render/vk pipeline.c - graphics pipeline (per swapchain format) */

#include "context.h"
#include "core/log.h"

int vk_create_pipeline(VkFormat format) {
  vk_context_t *ctx = vk_ctx();
  if (ctx->pipeline.vk_pipeline != VK_NULL_HANDLE && ctx->renderpass.vk_rp_format == format)
    return 0;
  if (ctx->pipeline.vk_pipeline != VK_NULL_HANDLE) {
    VK_LOAD(vkDestroyPipeline);
    vkDestroyPipeline(ctx->device.vk_dev, ctx->pipeline.vk_pipeline, NULL);
    ctx->pipeline.vk_pipeline = VK_NULL_HANDLE;
  }

  if (vk_create_render_pass(format) != 0)
    return -1;

  VkShaderModule vert_mod, frag_mod;
  if (vk_create_shader_modules(&vert_mod, &frag_mod) != 0)
    return -1;

  VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vert_mod,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = frag_mod,
       .pName = "main"},
  };

  VkPipelineVertexInputStateCreateInfo vis_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  VkPipelineInputAssemblyStateCreateInfo ias_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkPipelineViewportStateCreateInfo vps_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
  };
  VkPipelineRasterizationStateCreateInfo rs_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
  };
  VkPipelineMultisampleStateCreateInfo ms_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };

  VkPipelineColorBlendAttachmentState att_blend = {
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask =
          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  VkPipelineColorBlendStateCreateInfo cbs_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &att_blend,
  };

  VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dyn_states,
  };

  VkGraphicsPipelineCreateInfo pipe_ci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vis_ci,
      .pInputAssemblyState = &ias_ci,
      .pViewportState = &vps_ci,
      .pRasterizationState = &rs_ci,
      .pMultisampleState = &ms_ci,
      .pColorBlendState = &cbs_ci,
      .pDynamicState = &ds_ci,
      .layout = ctx->pipeline.vk_pll,
      .renderPass = ctx->renderpass.vk_render_pass,
      .subpass = 0,
  };

  VK_LOAD(vkCreateGraphicsPipelines);
  VkResult r =
      vkCreateGraphicsPipelines(ctx->device.vk_dev, VK_NULL_HANDLE, 1, &pipe_ci, NULL, &ctx->pipeline.vk_pipeline);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "CreateGraphicsPipelines failed: %d\n", r);
    return -1;
  }

  VK_LOAD(vkDestroyShaderModule);
  vkDestroyShaderModule(ctx->device.vk_dev, vert_mod, NULL);
  vkDestroyShaderModule(ctx->device.vk_dev, frag_mod, NULL);

  IDK_LOG("comp-vk", "Pipeline created\n");
  return 0;
}
