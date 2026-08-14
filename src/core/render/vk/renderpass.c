/* render/vk renderpass.c - render pass (per swapchain format) + per-frame setup */

#include "context.h"
#include "core/log.h"

int vk_create_render_pass(VkFormat format) {
  vk_context_t *ctx = vk_ctx();
  if (ctx->renderpass.vk_render_pass != VK_NULL_HANDLE && ctx->renderpass.vk_rp_format == format)
    return 0;

  VK_LOAD(vkDestroyRenderPass);
  if (ctx->renderpass.vk_render_pass != VK_NULL_HANDLE) {
    vkDestroyRenderPass(ctx->device.vk_dev, ctx->renderpass.vk_render_pass, NULL);
    ctx->renderpass.vk_render_pass = VK_NULL_HANDLE;
  }

  VkAttachmentDescription att = {
      .format = format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
  };
  VkAttachmentReference ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };
  VkSubpassDescription sub = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &ref,
  };
  VkRenderPassCreateInfo rp_ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &att,
      .subpassCount = 1,
      .pSubpasses = &sub,
  };
  VK_LOAD(vkCreateRenderPass);
  VkResult r = vkCreateRenderPass(ctx->device.vk_dev, &rp_ci, NULL, &ctx->renderpass.vk_render_pass);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "CreateRenderPass failed: %d\n", r);
    return -1;
  }

  ctx->renderpass.vk_rp_format = format;
  IDK_LOG("comp-vk", "RenderPass created (format=%d)\n", format);

  VK_LOAD(vkDestroyPipeline);
  if (ctx->pipeline.vk_pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(ctx->device.vk_dev, ctx->pipeline.vk_pipeline, NULL);
    ctx->pipeline.vk_pipeline = VK_NULL_HANDLE;
  }

  return 0;
}

/* Destroy the render pass + reset the cached format (shutdown). */
void renderpass_destroy(void) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkDestroyRenderPass);
  if (ctx->renderpass.vk_render_pass != VK_NULL_HANDLE)
    vkDestroyRenderPass(ctx->device.vk_dev, ctx->renderpass.vk_render_pass, NULL);
  ctx->renderpass.vk_render_pass = VK_NULL_HANDLE;
  ctx->renderpass.vk_rp_format = VK_FORMAT_UNDEFINED;
}

/* Per-frame draw setup: bind the current overlay view into the descriptor
 * set, then create the swapchain image view + framebuffer (recreated every
 * frame because the swapchain image changes). Returns 0 on success. */
int renderpass_make_framebuffer(VkImage img, uint32_t w, uint32_t h, VkFramebuffer *fb, VkImageView *view) {
  vk_context_t *ctx = vk_ctx();
  VK_LOAD(vkUpdateDescriptorSets);
  VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = ctx->pipeline.vk_desc_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo =
          &(VkDescriptorImageInfo){
              .sampler = ctx->pipeline.vk_sampler,
              .imageView = ctx->overlay.vk_overlay_view,
              .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          },
  };
  vkUpdateDescriptorSets(ctx->device.vk_dev, 1, &write, 0, NULL);

  VK_LOAD(vkCreateFramebuffer);
  VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = ctx->renderpass.vk_rp_format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  VkImageView swapchain_view;
  if (vkCreateImageView(ctx->device.vk_dev, &vci, NULL, &swapchain_view) != VK_SUCCESS) {
    return -1;
  }

  VkFramebufferCreateInfo fbci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = ctx->renderpass.vk_render_pass,
      .attachmentCount = 1,
      .pAttachments = &swapchain_view,
      .width = w,
      .height = h,
      .layers = 1,
  };
  VkFramebuffer new_fb;
  if (vkCreateFramebuffer(ctx->device.vk_dev, &fbci, NULL, &new_fb) != VK_SUCCESS) {
    vkDestroyImageView(ctx->device.vk_dev, swapchain_view, NULL);
    return -1;
  }
  *view = swapchain_view;
  *fb = new_fb;
  return 0;
}
