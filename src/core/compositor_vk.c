/* compositor_vk.c — Vulkan-native compositor
 *
 * Pure Vulkan rendering pipeline for overlay. No GL/EGL dependency.
 *
 * Receives overlay frames (SHM or DMABUF fd) from webview via socket,
 * imports to VkImage, renders fullscreen overlay on swapchain image.
 *
 * DMABUF: zero-copy import via VK_KHR_external_memory_fd
 * SHM: staging buffer upload via vkCmdCopyBufferToImage
 */

#ifdef IDK_HAVE_VK_LAYER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <vulkan/vulkan.h>

#include "core/compositor_vk.h"
#include "core/compositor_common.h"
#include "core/transport.h"
#include "core/log.h"
#include "shaders/vk_shaders.h"

/* ── SPIR-V shader bytecode ──────────────────────────────────────────────
 * Generated from overlay_vk.vert / overlay_vk.frag via glslc, embedded as
 * object files via ld -b binary + objcopy (same flow as GL SPIR-V shaders).
 *
 * Symbols (declared extern in include/shaders/vk_shaders.h, linked via
 * spv_o_files in meson.build):
 *   spv_overlay_vk_vert, spv_overlay_vk_vert_size
 *   spv_overlay_vk_frag, spv_overlay_vk_frag_size
 *
 * Vertex: fullscreen triangle (3 vertices, no VBO needed)
 * Fragment: sample texture, output premultiplied RGBA */

/* Frame protocol via compositor_common.h (idk_ipc.h). */

/* ── Transport state ──────────────────────────────────────────────────── */

static char vk_sock_path[512];
static idk_transport_t vk_tp;

static int vk_sock_init(void) {
    idk_comp_get_path(vk_sock_path, sizeof(vk_sock_path));
    return idk_tp_init(&vk_tp, IDK_TP_CONSUMER, vk_sock_path);
}

static void vk_sock_accept(void) {
    idk_tp_accept(&vk_tp);
}

/* ── ACK + resize state ───────────────────────────────────────────────── */

static int vk_game_w = 0, vk_game_h = 0;
static bool vk_size_pending = false;
/* Set PER-FRAME when this frame's DMABUF import failed. The ACK for this
 * frame will carry ack=1 to tell the webview "this DMABUF didn't work".
 *
 * NOT latched — a transient failure (e.g. first frame after resize when
 * Qt RHI's texture isn't fully rebuilt yet) should NOT permanently disable
 * DMABUF. The webview tracks consecutive ack=1 count and only falls back
 * to SHM after N consecutive failures. */
static int vk_dmabuf_failed_this_frame = 0;
static struct timespec vk_last_resize_ts = {0};

/* Swapchain recreation storm protection.
 * When the game rapidly recreates swapchains (vkcube does this on resize),
 * our render_overlay hook runs synchronously inside QueuePresentKHR and
 * blocks the game. The synchronous vkQueueSubmit+vkWaitForFences takes
 * ~5-10ms per frame, and during a resize storm (10+ recreations/sec) this
 * causes ACK timeouts → broken pipe → game freezes.
 * Skip overlay rendering for SWAPCHAIN_COOLDOWN_MS after each swapchain
 * recreation. The game's present goes through unmodified, overlay returns
 * once the storm settles. */
static struct timespec vk_last_swapchain_create_ts = {0};
#define VK_SWAPCHAIN_COOLDOWN_MS 100

void idk_vk_compositor_notify_swapchain_created(void) {
    clock_gettime(CLOCK_MONOTONIC, &vk_last_swapchain_create_ts);
}

void idk_vk_compositor_notify_resize(int w, int h) {
    idk_comp_notify_resize(&vk_game_w, &vk_game_h, &vk_size_pending,
                           &vk_last_resize_ts, w, h, "comp-vk");
}

static void vk_send_ack(int processed) {
    uint8_t ack = (vk_dmabuf_failed_this_frame || processed < 0) ? 1 : 0;
    vk_dmabuf_failed_this_frame = 0;
    idk_ack_msg_t ack_msg;
    idk_comp_build_ack(&ack_msg, ack,
                       vk_game_w, vk_game_h,
                       &vk_size_pending, &vk_last_resize_ts,
                       IDK_COMP_RESIZE_DEBOUNCE_MS, "comp-vk");
    idk_tp_send_ack(&vk_tp, &ack_msg);
}

/* ── Vulkan device state ───────────────────────────────────────────────── */

static VkDevice           vk_dev = VK_NULL_HANDLE;
static VkPhysicalDevice   vk_phys = VK_NULL_HANDLE;
static uint32_t           vk_queue_family = 0;
static VkQueue            vk_queue = VK_NULL_HANDLE;
static VkCommandPool      vk_cmd_pool = VK_NULL_HANDLE;

/* GPU vendor info for cross-GPU dmabuf detection.
 * DRM modifier encodes vendor in bits 56-63:
 *   0x01 = Intel (I915), 0x02 = AMD, 0x03 = NVIDIA, 0x04 = Samsung, etc.
 * VkPhysicalDeviceProperties.vendorID:
 *   0x10DE = NVIDIA, 0x8086 = Intel, 0x1002 = AMD
 * We map Vk vendorID → DRM vendor ID and compare with dmabuf modifier's
 * vendor bits. If mismatch → dmabuf was produced by a different GPU vendor
 * → cross-GPU import will likely produce garbage → reject, force SHM. */
static uint32_t           vk_vk_vendor_id = 0;     /* VkPhysicalDeviceProperties.vendorID */
static uint32_t           vk_drm_vendor_id = 0;    /* mapped DRM modifier vendor (0=unknown) */
/* vk_vendor_to_drm and DRM_MOD_VENDOR are in compositor_common.h */

/* Pipeline objects */
static VkDescriptorSetLayout vk_dsl = VK_NULL_HANDLE;
static VkPipelineLayout      vk_pll = VK_NULL_HANDLE;
static VkPipeline            vk_pipeline = VK_NULL_HANDLE;
static VkSampler             vk_sampler = VK_NULL_HANDLE;
static VkDescriptorPool      vk_desc_pool = VK_NULL_HANDLE;
static VkDescriptorSet       vk_desc_set = VK_NULL_HANDLE;

/* Render pass (per swapchain format) */
static VkRenderPass    vk_render_pass = VK_NULL_HANDLE;
static VkFormat        vk_rp_format = VK_FORMAT_UNDEFINED;

/* Framebuffer (per render call — created/destroyed each frame) */
/* Actually we create framebuffer per-frame because swapchain image changes */

/* Current overlay texture */
static VkImage         vk_overlay_img = VK_NULL_HANDLE;
static VkImageView     vk_overlay_view = VK_NULL_HANDLE;
static uint32_t        vk_overlay_w = 0;   /* current frame's width (from webview) */
static uint32_t        vk_overlay_h = 0;   /* current frame's height (from webview) */
static uint32_t        vk_shm_img_w = 0;   /* actual allocated VkImage width */
static uint32_t        vk_shm_img_h = 0;   /* actual allocated VkImage height */
static int             vk_has_frame = 0;

/* SHM staging */
static VkBuffer        vk_staging_buf = VK_NULL_HANDLE;
static VkDeviceMemory  vk_staging_mem = VK_NULL_HANDLE;
static void           *vk_staging_mapped = NULL;
static VkDeviceSize    vk_staging_size = 0;
static VkImage         vk_shm_img = VK_NULL_HANDLE;
static VkDeviceMemory  vk_shm_img_mem = VK_NULL_HANDLE;
static VkImageView     vk_shm_view = VK_NULL_HANDLE;

/* SHM mmap (for reading frame data) */
static int    vk_shm_fd = -1;

/* DMABUF import state.
 * Unlike SHM (which is uploaded every frame via staging buffer),
 * DMABUF is imported ONCE per (fd, w, h, stride, format, modifier) tuple
 * and the imported VkImage is sampled directly. The dmabuf fd MUST stay
 * open for the lifetime of the imported VkImage — closing it before
 * vkDestroyImage may cause GPU memory corruption. */
static int           vk_dmabuf_fd = -1;       /* currently-imported dmabuf fd */
static uint32_t      vk_dmabuf_w = 0;
static uint32_t      vk_dmabuf_h = 0;
static uint32_t      vk_dmabuf_stride = 0;
static uint32_t      vk_dmabuf_fourcc = 0;
static uint64_t      vk_dmabuf_modifier = 0;
static VkImage       vk_dmabuf_img = VK_NULL_HANDLE;
static VkDeviceMemory vk_dmabuf_img_mem = VK_NULL_HANDLE;
static VkImageView   vk_dmabuf_view = VK_NULL_HANDLE;

/* "Pending" dmabuf — received but not yet imported (import happens in
 * render_overlay because we need a command buffer for layout transition). */
static int           vk_dmabuf_pending_fd = -1;
static uint32_t      vk_dmabuf_pending_w = 0;
static uint32_t      vk_dmabuf_pending_h = 0;
static uint32_t      vk_dmabuf_pending_stride = 0;
static uint32_t      vk_dmabuf_pending_fourcc = 0;
static uint64_t      vk_dmabuf_pending_modifier = 0;
static int           vk_has_dmabuf_pending = 0;

/* Dispatch table — function pointers resolved via gpa */
static PFN_vkGetDeviceProcAddr vk_gpa = NULL;

/* Helper macro to load device functions.
 * NOTE: vkGetPhysicalDeviceMemoryProperties is an INSTANCE-level function,
 * not device-level. Loading it via vkGetDeviceProcAddr returns NULL.
 * We need to load it via vkGetInstanceProcAddr instead. */
#define VK_LOAD(name) static PFN_##name name = NULL; \
    if (!name) name = (PFN_##name)vk_gpa(vk_dev, #name)

/* Instance-level functions loaded once from the layer's GetInstanceProcAddr */
static PFN_vkGetPhysicalDeviceMemoryProperties vk_fn_GetPhysMemProps = NULL;
static PFN_vkGetInstanceProcAddr vk_fn_GetInstanceProcAddr = NULL;

void comp_vk_set_instance_gpa(PFN_vkGetInstanceProcAddr gpa) {
    vk_fn_GetInstanceProcAddr = gpa;
    if (gpa && !vk_fn_GetPhysMemProps)
        vk_fn_GetPhysMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)gpa(NULL, "vkGetPhysicalDeviceMemoryProperties");
}

/* ── Init pipeline ─────────────────────────────────────────────────────── */

static int vk_create_pipeline_objects(void) {
    VkResult r;

    /* Descriptor set layout: 1 binding — sampled image */
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
    r = vkCreateDescriptorSetLayout(vk_dev, &dsl_ci, NULL, &vk_dsl);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "CreateDescriptorSetLayout failed: %d\n", r); return -1; }

    /* Pipeline layout */
    VkPipelineLayoutCreateInfo pll_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vk_dsl,
    };
    VK_LOAD(vkCreatePipelineLayout);
    r = vkCreatePipelineLayout(vk_dev, &pll_ci, NULL, &vk_pll);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "CreatePipelineLayout failed: %d\n", r); return -1; }

    /* Sampler */
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
    r = vkCreateSampler(vk_dev, &samp_ci, NULL, &vk_sampler);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "CreateSampler failed: %d\n", r); return -1; }

    /* Descriptor pool */
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
    r = vkCreateDescriptorPool(vk_dev, &pool_ci, NULL, &vk_desc_pool);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "CreateDescriptorPool failed: %d\n", r); return -1; }

    /* Allocate descriptor set */
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vk_desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vk_dsl,
    };
    VK_LOAD(vkAllocateDescriptorSets);
    r = vkAllocateDescriptorSets(vk_dev, &ds_ai, &vk_desc_set);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "AllocateDescriptorSets failed: %d\n", r); return -1; }

    /* Command pool */
    VkCommandPoolCreateInfo cmd_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk_queue_family,
    };
    VK_LOAD(vkCreateCommandPool);
    r = vkCreateCommandPool(vk_dev, &cmd_ci, NULL, &vk_cmd_pool);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "CreateCommandPool failed: %d\n", r); return -1; }

    IDK_LOG("comp-vk", "Pipeline objects created\n");
    return 0;
}

static int vk_create_render_pass(VkFormat format) {
    if (vk_render_pass != VK_NULL_HANDLE && vk_rp_format == format) return 0;

    VK_LOAD(vkDestroyRenderPass);
    if (vk_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(vk_dev, vk_render_pass, NULL);
        vk_render_pass = VK_NULL_HANDLE;
    }

    /* CRITICAL: the swapchain image arrives from vkAcquireNextImageKHR in
     * PRESENT_SRC_KHR layout, and the present operation requires it back
     * in PRESENT_SRC_KHR. Using COLOR_ATTACHMENT_OPTIMAL here causes a
     * layout mismatch → undefined behavior → garbled colors.
     *
     * The render pass performs the layout transitions:
     *   PRESENT_SRC_KHR (initial) → COLOR_ATTACHMENT_OPTIMAL (subpass)
     *   → PRESENT_SRC_KHR (final)
     * loadOp=LOAD preserves the game's rendered content through the
     * initial transition so we can alpha-blend our overlay on top. */
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
    VkResult r = vkCreateRenderPass(vk_dev, &rp_ci, NULL, &vk_render_pass);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "CreateRenderPass failed: %d\n", r); return -1; }

    vk_rp_format = format;
    IDK_LOG("comp-vk", "RenderPass created (format=%d)\n", format);

    /* Recreate pipeline if it exists */
    VK_LOAD(vkDestroyPipeline);
    if (vk_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_dev, vk_pipeline, NULL);
        vk_pipeline = VK_NULL_HANDLE;
    }

    return 0;
}

static int vk_create_pipeline(VkFormat format) {
    /* Recreate pipeline if format changed (render_pass handles its own
     * destruction internally when format differs from cached vk_rp_format). */
    if (vk_pipeline != VK_NULL_HANDLE && vk_rp_format == format) return 0;
    if (vk_pipeline != VK_NULL_HANDLE) {
        VK_LOAD(vkDestroyPipeline);
        vkDestroyPipeline(vk_dev, vk_pipeline, NULL);
        vk_pipeline = VK_NULL_HANDLE;
    }

    if (vk_create_render_pass(format) != 0) return -1;

    /* Shader modules — SPIR-V bytecode embedded via ld -b binary (see
     * include/shaders/vk_shaders.h). Symbols resolved at link time from
     * spv_o_files in meson.build. */
#ifdef HAS_VK_SPV
    VK_LOAD(vkCreateShaderModule);
    VkShaderModuleCreateInfo vert_ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = VK_SPV_SHADER_SIZE(vert),
        .pCode = (const uint32_t *)spv_overlay_vk_vert,
    };
    VkShaderModule vert_mod;
    VkResult r = vkCreateShaderModule(vk_dev, &vert_ci, NULL, &vert_mod);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "vert shader module failed: %d\n", r); return -1; }

    VkShaderModuleCreateInfo frag_ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = VK_SPV_SHADER_SIZE(frag),
        .pCode = (const uint32_t *)spv_overlay_vk_frag,
    };
    VkShaderModule frag_mod;
    r = vkCreateShaderModule(vk_dev, &frag_ci, NULL, &frag_mod);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "frag shader module failed: %d\n", r); return -1; }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert_mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag_mod, .pName = "main" },
    };

    /* No vertex input — fullscreen triangle generated in shader */
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

    /* Premultiplied alpha blend */
    VkPipelineColorBlendAttachmentState att_blend = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cbs_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att_blend,
    };

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
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
        .layout = vk_pll,
        .renderPass = vk_render_pass,
        .subpass = 0,
    };

    VK_LOAD(vkCreateGraphicsPipelines);
    r = vkCreateGraphicsPipelines(vk_dev, VK_NULL_HANDLE, 1, &pipe_ci, NULL, &vk_pipeline);
    if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "CreateGraphicsPipelines failed: %d\n", r); return -1; }

    VK_LOAD(vkDestroyShaderModule);
    vkDestroyShaderModule(vk_dev, vert_mod, NULL);
    vkDestroyShaderModule(vk_dev, frag_mod, NULL);
#else
    IDK_ERR("comp-vk", "VK SPIR-V shaders not built (glslc missing) — cannot create pipeline\n");
    return -1;
#endif /* HAS_VK_SPV */

    IDK_LOG("comp-vk", "Pipeline created\n");
    return 0;
}

/* ── SHM upload ────────────────────────────────────────────────────────── */

static int vk_upload_shm(int fd, uint32_t w, uint32_t h, uint32_t pixel_size,
                          uint32_t buf_idx, VkCommandBuffer cmd) {
    VkResult r;
    VK_LOAD(vkCreateBuffer);
    VK_LOAD(vkAllocateMemory);
    VK_LOAD(vkBindBufferMemory);
    VK_LOAD(vkMapMemory);
    VK_LOAD(vkUnmapMemory);
    VK_LOAD(vkCreateImage);
    VK_LOAD(vkBindImageMemory);
    VK_LOAD(vkCreateImageView);
    VK_LOAD(vkDestroyBuffer);
    VK_LOAD(vkFreeMemory);
    VK_LOAD(vkCmdCopyBufferToImage);
    VK_LOAD(vkGetBufferMemoryRequirements);
    VK_LOAD(vkGetImageMemoryRequirements);
    VK_LOAD(vkFlushMappedMemoryRanges);
    VK_LOAD(vkCmdPipelineBarrier);

    if (!vk_fn_GetPhysMemProps) {
        IDK_ERR("comp-vk", "upload: vk_fn_GetPhysMemProps not loaded\n");
        return -1;
    }


    static idk_shm_cache_t s_vk_shm_cache;
    if (!idk_shm_cache_map(&s_vk_shm_cache, fd))
        return -1;

    uint8_t *data = (uint8_t *)s_vk_shm_cache.map + (buf_idx * pixel_size);
    VkDeviceSize buf_size = (VkDeviceSize)pixel_size;

    /* Create staging buffer */
    if (vk_staging_buf == VK_NULL_HANDLE || vk_staging_size < buf_size) {
        if (vk_staging_buf) { vkDestroyBuffer(vk_dev, vk_staging_buf, NULL); vk_staging_buf = VK_NULL_HANDLE; }
        /* Unmap and free old staging memory before reallocating (otherwise leak on every grow) */
        if (vk_staging_mapped) { vkUnmapMemory(vk_dev, vk_staging_mem); vk_staging_mapped = NULL; }
        if (vk_staging_mem) { vkFreeMemory(vk_dev, vk_staging_mem, NULL); vk_staging_mem = VK_NULL_HANDLE; }
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = buf_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        r = vkCreateBuffer(vk_dev, &bci, NULL, &vk_staging_buf);
        if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "upload: CreateBuffer failed: %d\n", r); return -1; }

        VkMemoryRequirements mr;

        vkGetBufferMemoryRequirements(vk_dev, vk_staging_buf, &mr);

        VkPhysicalDeviceMemoryProperties mp;
        vk_fn_GetPhysMemProps(vk_phys, &mp);

        /* Find HOST_VISIBLE memory */
        uint32_t mem_type = 0xFFFFFFFF;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            if ((mr.memoryTypeBits & (1 << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                mem_type = i;
                break;
            }
        }
        if (mem_type == 0xFFFFFFFF) { IDK_ERR("comp-vk", "upload: no HOST_VISIBLE mem type\n"); return -1; }

        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mem_type,
        };
        r = vkAllocateMemory(vk_dev, &mai, NULL, &vk_staging_mem);
        if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "upload: AllocateMemory(staging) failed: %d\n", r); return -1; }
        vkBindBufferMemory(vk_dev, vk_staging_buf, vk_staging_mem, 0);
        vkMapMemory(vk_dev, vk_staging_mem, 0, buf_size, 0, &vk_staging_mapped);
        vk_staging_size = buf_size;
    }

    /* Copy SHM data to staging buffer */
    memcpy(vk_staging_mapped, data, buf_size);

    VkMappedMemoryRange flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = vk_staging_mem,
        .size = VK_WHOLE_SIZE,
    };
    vkFlushMappedMemoryRanges(vk_dev, 1, &flush);

    /* Create destination VkImage if needed.
     * CRITICAL: compare against vk_shm_img_w/h (the actual image dimensions),
     * NOT against vk_overlay_w/h (which == w,h passed in). Comparing against
     * vk_overlay_w/h would make this condition always false → image never
     * recreated → vkCmdCopyBufferToImage writes past image bounds → GPU
     * memory corruption → garbled overlay in corner of screen. */
    if (vk_shm_img == VK_NULL_HANDLE || vk_shm_img_w != w || vk_shm_img_h != h) {
        if (vk_shm_img) { vkDestroyImage(vk_dev, vk_shm_img, NULL); vk_shm_img = VK_NULL_HANDLE; }
        if (vk_shm_view) { vkDestroyImageView(vk_dev, vk_shm_view, NULL); vk_shm_view = VK_NULL_HANDLE; }
        /* Free old image memory before reallocating (otherwise leak on every resize) */
        if (vk_shm_img_mem) { vkFreeMemory(vk_dev, vk_shm_img_mem, NULL); vk_shm_img_mem = VK_NULL_HANDLE; }

        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { w, h, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        r = vkCreateImage(vk_dev, &ici, NULL, &vk_shm_img);
        if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "upload: CreateImage failed: %d\n", r); return -1; }

        VkMemoryRequirements mr;

        vkGetImageMemoryRequirements(vk_dev, vk_shm_img, &mr);

        VkPhysicalDeviceMemoryProperties mp;
        vk_fn_GetPhysMemProps(vk_phys, &mp);
        uint32_t mem_type = 0xFFFFFFFF;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            if ((mr.memoryTypeBits & (1 << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                mem_type = i;
                break;
            }
        }
        if (mem_type == 0xFFFFFFFF) { IDK_ERR("comp-vk", "upload: no DEVICE_LOCAL mem type\n"); return -1; }

        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mem_type,
        };
        r = vkAllocateMemory(vk_dev, &mai, NULL, &vk_shm_img_mem);
        if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "upload: AllocateMemory(image) failed: %d\n", r); return -1; }
        vkBindImageMemory(vk_dev, vk_shm_img, vk_shm_img_mem, 0);

        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = vk_shm_img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        r = vkCreateImageView(vk_dev, &vci, NULL, &vk_shm_view);
        if (r != VK_SUCCESS) { IDK_ERR("comp-vk", "upload: CreateImageView failed: %d\n", r); return -1; }

        vk_shm_img_w = w;
        vk_shm_img_h = h;
        IDK_LOG("comp-vk", "upload: VkImage recreated (%ux%u)\n", w, h);
    }

    /* Transition image to TRANSFER_DST */

    VkImageMemoryBarrier bar = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = vk_shm_img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);

    /* Copy buffer to image */
    VkBufferImageCopy copy = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { w, h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, vk_staging_buf, vk_shm_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    /* Transition to SHADER_READ_ONLY */
    VkImageMemoryBarrier bar2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = vk_shm_img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &bar2);

    vk_overlay_img = vk_shm_img;
    vk_overlay_view = vk_shm_view;
    return 0;
}

/* ── DMABUF import ────────────────────────────────────────────────────── */

/* Map DRM fourcc → VkFormat.
 *
 * DRM fourcc convention: fourcc_code(a,b,c,d) = a | (b<<8) | (c<<16) | (d<<24).
 * The 4-char name describes the PIXEL as a 32-bit word: MSB→LSB = first→last char.
 * "ARGB8888" = word [31:0] = A:R:G:B (A in high bits, B in low bits).
 *
 * In little-endian memory, the LOW byte of the word is at the LOW address.
 * So DRM_FORMAT_ARGB8888 (word A:R:G:B) → memory bytes B,G,R,A (low→high).
 * VkFormat naming uses MEMORY byte order: B8G8R8A8 = bytes B,G,R,A in memory.
 *
 * Therefore:
 *   DRM_FORMAT_ARGB8888 (A:R:G:B word) ↔ Vk B8G8R8A8_UNORM (B,G,R,A memory)
 *   DRM_FORMAT_ABGR8888 (A:B:G:R word) ↔ Vk R8G8B8A8_UNORM (R,G,B,A memory)
 *   DRM_FORMAT_RGBA8888 (R:G:B:A word) ↔ Vk A8B8G8R8_UNORM_PACK32 (A,B,G,R memory)
 *   DRM_FORMAT_BGRA8888 (B:G:R:A word) — A in high byte, BGR in low bytes.
 *     Memory: B,G,R,A. Map to B8G8R8A8_UNORM (Vulkan has no A8R8G8B8_UNORM_PACK32,
 *     but B8G8R8A8 has the same memory layout B,G,R,A — the format name describes
 *     the same byte order, just different component-naming convention).
 *
 * Qt RHI on Mesa exports GL_RGBA8 textures as DRM_FORMAT_ABGR8888 ("AB24" =
 * fourcc_code('A','B','2','4') = 0x34324241). GL_RGBA8 stores R,G,B,A in
 * memory, which matches VkFormat R8G8B8A8_UNORM. Confirmed working on NVIDIA. */
static VkFormat drm_fourcc_to_vk_format(uint32_t fourcc) {
    /* "AB24" (0x34324241) = DRM_FORMAT_ABGR8888 — A:B:G:R word = R,G,B,A memory.
     * Qt RHI GL_RGBA8 default. Map to R8G8B8A8_UNORM. */
    if (fourcc == 0x34324241u) return VK_FORMAT_R8G8B8A8_UNORM;
    /* "AR24" (0x34325241) = DRM_FORMAT_ARGB8888 — A:R:G:B word = B,G,R,A memory.
     * Map to B8G8R8A8_UNORM. */
    if (fourcc == 0x34325241u) return VK_FORMAT_B8G8R8A8_UNORM;
    /* "XB24" (0x34324258) = DRM_FORMAT_XBGR8888 — X:B:G:R word = R,G,B,X memory.
     * Map to R8G8B8A8_UNORM (alpha ignored). */
    if (fourcc == 0x34324258u) return VK_FORMAT_R8G8B8A8_UNORM;
    /* "XR24" (0x34325258) = DRM_FORMAT_XRGB8888 — X:R:G:B word = B,G,R,X memory.
     * Map to B8G8R8A8_UNORM (alpha ignored). */
    if (fourcc == 0x34325258u) return VK_FORMAT_B8G8R8A8_UNORM;
    /* "RA24" (0x34324152) = DRM_FORMAT_RGBA8888 — R:G:B:A word = A,B,G,R memory. */
    if (fourcc == 0x34324152u) return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    /* "BA24" (0x34324142) = DRM_FORMAT_BGRA8888 — B:G:R:A word.
     * Memory: B,G,R,A (same as ARGB8888). Map to B8G8R8A8_UNORM. */
    if (fourcc == 0x34324142u) return VK_FORMAT_B8G8R8A8_UNORM;
    /* Fallback: assume ABGR8888 (Qt RHI's GL_RGBA8 default on Mesa). */
    return VK_FORMAT_R8G8B8A8_UNORM;
}

/* IDK_DRM_FORMAT_MOD_INVALID is in compositor_common.h */

/* Import a dmabuf fd as a VkImage and create a sampled view.
 * Returns 0 on success, -1 on failure. The fd is owned by the caller —
 * the imported VkImage holds its own reference (the underlying memory
 * remains valid as long as either the fd OR the VkImage is alive, but
 * we MUST keep the fd open to be safe — see vk_dmabuf_fd tracking). */
static int vk_upload_dmabuf(int fd, uint32_t w, uint32_t h, uint32_t stride,
                              uint32_t fourcc, uint64_t modifier,
                              VkCommandBuffer cmd) {
    VkResult r;
    VK_LOAD(vkCreateImage);
    VK_LOAD(vkAllocateMemory);
    VK_LOAD(vkBindImageMemory);
    VK_LOAD(vkCreateImageView);
    VK_LOAD(vkDestroyImage);
    VK_LOAD(vkDestroyImageView);
    VK_LOAD(vkFreeMemory);
    VK_LOAD(vkGetMemoryFdPropertiesKHR);
    VK_LOAD(vkCmdPipelineBarrier);

    /* Cross-GPU vendor check: if the dmabuf modifier's vendor doesn't match
     * our GPU vendor, the dmabuf was produced by a different GPU (e.g.
     * webview on Intel iGPU, compositor on NVIDIA dGPU). Cross-vendor
     * dmabuf import with tiled modifiers produces garbage because the
     * consumer driver can't interpret the producer's tiling layout.
     * Reject immediately and force SHM fallback.
     *
     * Modifier=0 (linear) or INVALID bypasses this check — linear dmabufs
     * are vendor-agnostic and can be imported by any driver. */
    if (vk_drm_vendor_id != 0 && modifier != 0 && modifier != IDK_DRM_FORMAT_MOD_INVALID) {
        uint32_t mod_vendor = IDK_DRM_MOD_VENDOR(modifier);
        if (mod_vendor != vk_drm_vendor_id) {
            IDK_LOG("comp-vk", "dmabuf: cross-GPU vendor mismatch (modifier vendor=0x%02x, our vendor=0x%02x) — rejecting, force SHM\n",
                    mod_vendor, vk_drm_vendor_id);
            vk_dmabuf_failed_this_frame = 1;
            /* fd NOT consumed by ICD — caller will close it. */
            return -1;
        }
    }

    /* If we already have an import for this exact frame identity, skip.
     * NOTE: Qt reuses the same dmabuf fd across frames only sometimes —
     * more often it creates a new fd per frame. So this cache rarely hits,
     * but when it does, we save a full vkCreateImage+AllocateMemory cycle. */
    if (vk_dmabuf_img != VK_NULL_HANDLE &&
        vk_dmabuf_w == w && vk_dmabuf_h == h &&
        vk_dmabuf_stride == stride && vk_dmabuf_fourcc == fourcc &&
        vk_dmabuf_modifier == modifier &&
        vk_dmabuf_fd == fd) {
        /* Same fd reused by Qt → image still valid, just transition layout. */
        vk_overlay_img = vk_dmabuf_img;
        vk_overlay_view = vk_dmabuf_view;
        /* Fall through to layout transition below. */
    } else {
        /* Tear down old import.
         * NOTE: vkFreeMemory closes the imported fd (ICD took ownership
         * via VkImportMemoryFdInfoKHR on successful vkAllocateMemory).
         * Do NOT close(vk_dmabuf_fd) — that would double-close. */
        if (vk_dmabuf_view) { vkDestroyImageView(vk_dev, vk_dmabuf_view, NULL); vk_dmabuf_view = VK_NULL_HANDLE; }
        if (vk_dmabuf_img)  { vkDestroyImage(vk_dev, vk_dmabuf_img, NULL);  vk_dmabuf_img  = VK_NULL_HANDLE; }
        if (vk_dmabuf_img_mem) { vkFreeMemory(vk_dev, vk_dmabuf_img_mem, NULL); vk_dmabuf_img_mem = VK_NULL_HANDLE; }
        vk_dmabuf_fd = -1;  /* ICD already closed it via vkFreeMemory */

        VkFormat vk_fmt = drm_fourcc_to_vk_format(fourcc);

        /* ── Build pNext chain for VkImageCreateInfo ──
         *
         * DMABUF import requires VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT.
         * The modifier info can be passed in 3 ways:
         *   (a) VkImageDrmFormatModifierExplicitCreateInfoEXT — exact modifier + plane layouts
         *   (b) VkImageDrmFormatModifierListCreateInfoEXT — list of acceptable modifiers
         *   (c) No modifier struct — driver queries dmabuf's modifier via kernel
         *
         * APPROACH: use (a) with explicit modifier + plane layout. This was
         * confirmed working on NVIDIA (v7). For Intel X-tiled, the stride from
         * eglExportDMABUFImageMESA is already tile-aligned (verified: 7680=512*15,
         * 2048=512*4, 3072=512*6), so VkSubresourceLayout.rowPitch=stride is correct.
         *
         * (c) doesn't work reliably — without the modifier struct, drivers may
         * default to LINEAR tiling instead of querying the kernel, causing
         * corruption on tiled dmabufs (Intel X-tiled, NVIDIA block-linear). */
        /* VkSubresourceLayout for the dmabuf plane.
         * size = stride * h (entire image). Setting size=0 ("entire plane")
         * is allowed by spec but Intel ANV interprets it incorrectly —
         * sampling returns garbage. Use the actual computed size instead. */
        VkSubresourceLayout drm_layout = {
            .offset = 0,
            .size = (VkDeviceSize)stride * (VkDeviceSize)h,
            .rowPitch = stride,
            .arrayPitch = 0,
            .depthPitch = 0,
        };
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod_explicit = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = modifier,
            .drmFormatModifierPlaneCount = 1,  /* single-plane format */
            .pPlaneLayouts = &drm_layout,
        };

        int use_explicit = (modifier != 0 && modifier != IDK_DRM_FORMAT_MOD_INVALID);

        VkExternalMemoryImageCreateInfo ext_mem_ci = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = use_explicit ? &mod_explicit : NULL,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };

        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ext_mem_ci,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = vk_fmt,
            .extent = { w, h, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = use_explicit
                      ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                      : VK_IMAGE_TILING_LINEAR,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
        };

        r = vkCreateImage(vk_dev, &ici, NULL, &vk_dmabuf_img);
        if (r != VK_SUCCESS && use_explicit) {
            /* Fall back to LINEAR tiling — last resort, may not work for tiled. */
            IDK_LOG("comp-vk", "dmabuf: explicit modifier import failed (%d), trying LINEAR\n", r);
            ext_mem_ci.pNext = NULL;
            ici.tiling = VK_IMAGE_TILING_LINEAR;
            r = vkCreateImage(vk_dev, &ici, NULL, &vk_dmabuf_img);
        }
        if (r != VK_SUCCESS) {
            IDK_ERR("comp-vk", "dmabuf: CreateImage failed: %d (fmt=0x%x mod=0x%lx) — frame rejected\n",
                    r, fourcc, (unsigned long)modifier);
            vk_dmabuf_failed_this_frame = 1;
            /* fd NOT consumed by ICD — caller will close it. */
            return -1;
        }

        /* Get memory requirements + dmabuf fd properties to pick the right
         * memory type. vkGetMemoryFdPropertiesKHR tells us which memory types
         * are compatible with the dmabuf fd. */
        VK_LOAD(vkGetImageMemoryRequirements);
        VkMemoryRequirements2 mr2 = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        };
        VkImageMemoryRequirementsInfo2 mr_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = vk_dmabuf_img,
        };
        VK_LOAD(vkGetImageMemoryRequirements2);
        if (vkGetImageMemoryRequirements2) {
            vkGetImageMemoryRequirements2(vk_dev, &mr_info, &mr2);
        } else {
            vkGetImageMemoryRequirements(vk_dev, vk_dmabuf_img, &mr2.memoryRequirements);
        }

        VkMemoryFdPropertiesKHR fd_props = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
            .memoryTypeBits = 0,
        };
        r = vkGetMemoryFdPropertiesKHR(vk_dev,
                                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                        fd, &fd_props);
        if (r != VK_SUCCESS) {
            IDK_ERR("comp-vk", "dmabuf: GetMemoryFdPropertiesKHR failed: %d — frame rejected\n", r);
            vkDestroyImage(vk_dev, vk_dmabuf_img, NULL);
            vk_dmabuf_img = VK_NULL_HANDLE;
            vk_dmabuf_failed_this_frame = 1;
            /* fd NOT consumed by ICD — caller will close it. */
            return -1;
        }

        /* Intersect: image's allowed types AND fd's allowed types AND DEVICE_LOCAL. */
        if (!vk_fn_GetPhysMemProps) {
            IDK_ERR("comp-vk", "dmabuf: vk_fn_GetPhysMemProps not loaded\n");
            vkDestroyImage(vk_dev, vk_dmabuf_img, NULL);
            vk_dmabuf_img = VK_NULL_HANDLE;
            /* fd NOT consumed by ICD — caller will close it. */
            return -1;
        }
        VkPhysicalDeviceMemoryProperties mp;
        vk_fn_GetPhysMemProps(vk_phys, &mp);

        uint32_t allowed = mr2.memoryRequirements.memoryTypeBits & fd_props.memoryTypeBits;
        uint32_t mem_type = 0xFFFFFFFF;
        /* Prefer DEVICE_LOCAL (for sampling performance), fall back to any allowed. */
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            if ((allowed & (1 << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                mem_type = i;
                break;
            }
        }
        if (mem_type == 0xFFFFFFFF) {
            for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
                if (allowed & (1 << i)) { mem_type = i; break; }
            }
        }
        if (mem_type == 0xFFFFFFFF) {
            IDK_ERR("comp-vk", "dmabuf: no compatible mem type (image=0x%x fd=0x%x) — frame rejected\n",
                    mr2.memoryRequirements.memoryTypeBits, fd_props.memoryTypeBits);
            vkDestroyImage(vk_dev, vk_dmabuf_img, NULL);
            vk_dmabuf_img = VK_NULL_HANDLE;
            vk_dmabuf_failed_this_frame = 1;
            /* fd NOT consumed by ICD — caller will close it. */
            return -1;
        }

        /* Allocate device memory and import the dmabuf fd into it. */
        VkImportMemoryFdInfoKHR import_info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = NULL,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd = fd,  /* ICD takes ownership on success — see note below */
        };
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import_info,
            .allocationSize = mr2.memoryRequirements.size,
            .memoryTypeIndex = mem_type,
        };
        r = vkAllocateMemory(vk_dev, &mai, NULL, &vk_dmabuf_img_mem);
        if (r != VK_SUCCESS) {
            IDK_ERR("comp-vk", "dmabuf: AllocateMemory(import) failed: %d — frame rejected\n", r);
            vkDestroyImage(vk_dev, vk_dmabuf_img, NULL);
            vk_dmabuf_img = VK_NULL_HANDLE;
            vk_dmabuf_failed_this_frame = 1;
            /* fd NOT consumed by ICD (vkAllocateMemory failed) — caller will close it. */
            return -1;
        }
        /* ICD took ownership of fd on success → don't close it ourselves.
         * Track it so we don't double-import the same fd. */

        vkBindImageMemory(vk_dev, vk_dmabuf_img, vk_dmabuf_img_mem, 0);

        /* Create sampled view. */
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = vk_dmabuf_img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = vk_fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        r = vkCreateImageView(vk_dev, &vci, NULL, &vk_dmabuf_view);
        if (r != VK_SUCCESS) {
            IDK_ERR("comp-vk", "dmabuf: CreateImageView failed: %d\n", r);
            vkFreeMemory(vk_dev, vk_dmabuf_img_mem, NULL); vk_dmabuf_img_mem = VK_NULL_HANDLE;
            vkDestroyImage(vk_dev, vk_dmabuf_img, NULL); vk_dmabuf_img = VK_NULL_HANDLE;
            /* vkFreeMemory already closed the imported fd (ICD owned it
             * via successful vkAllocateMemory). Set vk_dmabuf_fd = -1
             * to signal caller NOT to close vk_dmabuf_pending_fd. */
            vk_dmabuf_fd = -1;
            return -1;
        }

        vk_dmabuf_w = w;
        vk_dmabuf_h = h;
        vk_dmabuf_stride = stride;
        vk_dmabuf_fourcc = fourcc;
        vk_dmabuf_modifier = modifier;
        vk_dmabuf_fd = fd;  /* track for cache hit detection */
        IDK_LOG("comp-vk", "dmabuf: imported %ux%u fourcc=0x%x mod=0x%lx stride=%u\n",
                w, h, fourcc, (unsigned long)modifier, stride);
    }

    /* Transition image to SHADER_READ_ONLY for sampling.
     *
     * For dmabuf imported from an external producer (Qt GL context in a
     * different process), we need to:
     * 1. Release ownership back to EXTERNAL (so producer can write)
     * 2. Acquire ownership from EXTERNAL (so we can read new content)
     *
     * On cache hit (same VkImage, Qt wrote new content to dmabuf), we skip
     * step 1 (already released from previous frame's present) and only do
     * step 2 — acquire with EXTERNAL src + our queue dst.
     *
     * On first import, the image is in PREINITIALIZED layout — acquire
     * transitions it to SHADER_READ_ONLY.
     *
     * srcAccessMask for acquire = 0 (the producer's writes are made visible
     * by the release barrier on THEIR side, which is the kernel's dma-buf
     * fence). dstAccessMask = SHADER_READ so our sampler sees the data.
     *
     * This is the canonical Vulkan external memory pattern. Both EXTERNAL
     * queue family indices must be set (not IGNORED) for the ownership
     * transfer to take effect — IGNORED would skip the transfer and leave
     * cached data stale on Intel ANV. */
    VkImageMemoryBarrier bar = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = vk_queue_family,
        .image = vk_dmabuf_img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &bar);

    vk_overlay_img = vk_dmabuf_img;
    vk_overlay_view = vk_dmabuf_view;
    return 0;
}

/* ── Frame receive ─────────────────────────────────────────────────────── */

/* Overlay visibility — same symbol as overlay.c's g_overlay_visible.
 * When 0, drain incoming frames without ACK/REQUEST so the webview
 * stops rendering (see compositor_egl.c for full rationale). */
extern volatile int g_overlay_visible;

/* Track visibility transitions so we can wake the webview up when the
 * overlay becomes visible again. */
static int vk_was_hidden = 0;

int idk_vk_compositor_render(void) {
    vk_sock_accept();
    if (!vk_tp.ready) return -1;

    /* When the overlay is hidden, drain any queued frames so the
     * producer's send queue doesn't accumulate, but DO NOT send ACK
     * or REQUEST. The webview's request-poll loop stays parked. */
    if (!g_overlay_visible) {
        while (1) {
            idk_frame_header_t hdr;
            int fds[4], nfd = 0;
            int rc = idk_tp_recv(&vk_tp, &hdr, fds, &nfd);
            if (rc <= 0) {
                if (rc < 0) {
                    idk_tp_destroy(&vk_tp);
                    vk_tp.ready = false;
                }
                break;
            }
            for (int i = 0; i < nfd; i++) close(fds[i]);
            IDK_LOG("comp-vk", "hidden: drained frame (nfd=%d) without ACK/REQUEST\n", nfd);
        }
        vk_was_hidden = 1;
        return -1;
    }

    /* Visible — if we just transitioned from hidden, send a REQUEST
     * to wake up the webview's request-poll loop. */
    if (vk_was_hidden) {
        vk_was_hidden = 0;
        idk_request_msg_t wake;
        memset(&wake, 0, sizeof(wake));
        wake.type = IDK_REQUEST_NEXT_FRAME;
        idk_tp_send_request(&vk_tp, &wake);
        IDK_LOG("comp-vk", "overlay became visible — sent wake-up REQUEST\n");
    }

    int processed = 0;
    while (1) {
        idk_frame_header_t hdr;
        int fds[4], nfd = 0;
        int rc = idk_tp_recv(&vk_tp, &hdr, fds, &nfd);
        if (rc <= 0) {
            if (rc < 0) {
                idk_tp_destroy(&vk_tp);
                vk_tp.ready = false;
            }
            break;
        }

        int fd = (nfd > 0) ? fds[0] : -1;

        if (processed > 0) { close(fd); continue; }

        if (!idk_frame_is_dmabuf(&hdr)) {
            /* SHM frame — will be uploaded in render_overlay via staging buffer */
            vk_overlay_w = hdr.width;
            vk_overlay_h = hdr.height;
            if (vk_shm_fd >= 0) close(vk_shm_fd);
            vk_shm_fd = fd;
            vk_has_frame = 1;
            if (vk_has_dmabuf_pending) {
                if (vk_dmabuf_pending_fd >= 0) close(vk_dmabuf_pending_fd);
                vk_has_dmabuf_pending = 0;
            }
            processed = 1;
        } else {
            /* DMABUF frame — stash for import in render_overlay.
             * fourcc from header tells the VK compositor which VkFormat
             * to use for VkImage creation. */
            if (vk_has_dmabuf_pending && vk_dmabuf_pending_fd >= 0) {
                close(vk_dmabuf_pending_fd);
            }
            vk_dmabuf_pending_fd = fd;
            vk_dmabuf_pending_w = hdr.width;
            vk_dmabuf_pending_h = hdr.height;
            vk_dmabuf_pending_stride = hdr.stride;
            vk_dmabuf_pending_fourcc = hdr.fourcc;
            vk_dmabuf_pending_modifier = hdr.modifier;
            vk_has_dmabuf_pending = 1;
            vk_has_frame = 1;
            if (vk_shm_fd >= 0) { close(vk_shm_fd); vk_shm_fd = -1; }
            processed = 1;
            IDK_LOG("comp-vk", "DMABUF pending: %ux%u stride=%u mod=0x%lx\n",
                    hdr.width, hdr.height,
                    hdr.stride, (unsigned long)hdr.modifier);
        }
    }

    if (processed) {
        vk_send_ack(processed);
        idk_request_msg_t req;
        memset(&req, 0, sizeof(req));
        req.type = IDK_REQUEST_NEXT_FRAME;
        idk_tp_send_request(&vk_tp, &req);
    }
    return processed > 0 ? 0 : -1;
}

/* ── Render overlay ────────────────────────────────────────────────────── */

void idk_vk_compositor_render_overlay(VkCommandBuffer cmd, VkImage swapchainImage,
                                      uint32_t width, uint32_t height,
                                      VkFormat swapchainFormat) {
    (void)cmd;  /* caller passes VK_NULL_HANDLE — we manage our own cmd buffer */
    if (!vk_has_frame || swapchainImage == VK_NULL_HANDLE) return;

    /* Skip overlay rendering during swapchain recreation storms.
     * Our synchronous vkQueueSubmit+vkWaitForFences blocks the game's
     * QueuePresentKHR, and during rapid resize the game can't keep up
     * → ACK timeout → broken pipe → game freezes (Intel bug).
     * Let the game present unmodified for SWAPCHAIN_COOLDOWN_MS after
     * each swapchain creation, then resume overlay rendering. */
    if (vk_last_swapchain_create_ts.tv_sec != 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long delta_ms = (now.tv_sec - vk_last_swapchain_create_ts.tv_sec) * 1000L
                      + (now.tv_nsec - vk_last_swapchain_create_ts.tv_nsec) / 1000000L;
        if (delta_ms < VK_SWAPCHAIN_COOLDOWN_MS) {
            return;  /* skip this frame — let game present unmodified */
        }
    }

    IDK_LOG("comp-vk", "render_overlay: has_frame=%d img=%p %ux%u fmt=%d\n",
            vk_has_frame, (void*)swapchainImage, width, height, (int)swapchainFormat);

    /* Create pipeline if needed — pass the actual swapchain format so the
     * render pass attachment format matches the swapchain image format.
     * Mismatched formats cause undefined behavior (garbled colors). */
    if (swapchainFormat == VK_FORMAT_UNDEFINED) {
        /* Fallback for safety — should never happen with a real swapchain */
        swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    }
    if (vk_pipeline == VK_NULL_HANDLE || vk_rp_format != swapchainFormat) {
        if (vk_create_pipeline(swapchainFormat) != 0) {
            IDK_ERR("comp-vk", "pipeline creation failed (fmt=%d)\n", (int)swapchainFormat);
            return;
        }
    }

    /* Allocate our own command buffer — the caller passes VK_NULL_HANDLE
     * because it doesn't manage command buffer lifecycle for us. */
    VK_LOAD(vkAllocateCommandBuffers);
    VK_LOAD(vkFreeCommandBuffers);
    VK_LOAD(vkBeginCommandBuffer);
    VK_LOAD(vkEndCommandBuffer);
    VK_LOAD(vkQueueSubmit);
    VK_LOAD(vkWaitForFences);
    VK_LOAD(vkResetFences);
    VK_LOAD(vkCreateFence);
    VK_LOAD(vkDestroyFence);

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer local_cmd;
    if (vkAllocateCommandBuffers(vk_dev, &cbai, &local_cmd) != VK_SUCCESS) {
        IDK_ERR("comp-vk", "allocate cmd buffer failed\n");
        return;
    }

    VkCommandBufferBeginInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(local_cmd, &cbi) != VK_SUCCESS) {
        vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &local_cmd);
        return;
    }

    VkCommandBuffer recording_cmd = local_cmd;

    /* Upload overlay texture.
     * DMABUF path: import the pending dmabuf fd as a VkImage (zero-copy).
     * SHM path: mmap + copy to staging buffer + vkCmdCopyBufferToImage.
     * DMABUF is preferred when available — no host→device copy. */
    if (vk_has_dmabuf_pending && vk_dmabuf_pending_fd >= 0) {
        int pending_fd = vk_dmabuf_pending_fd;
        vk_dmabuf_pending_fd = -1;  /* clear before call so vk_upload_dmabuf
                                     * can set vk_dmabuf_fd = -1 if ICD
                                     * consumed the fd */
        vk_has_dmabuf_pending = 0;

        if (vk_upload_dmabuf(pending_fd,
                              vk_dmabuf_pending_w, vk_dmabuf_pending_h,
                              vk_dmabuf_pending_stride,
                              vk_dmabuf_pending_fourcc,
                              vk_dmabuf_pending_modifier,
                              recording_cmd) != 0) {
            IDK_ERR("comp-vk", "render_overlay: dmabuf import failed (fd=%d)\n",
                    pending_fd);
            /* Only close fd if ICD didn't consume it.
             * vk_upload_dmabuf sets vk_dmabuf_fd = -1 when ICD takes
             * ownership (vkAllocateMemory succeeded). If vk_dmabuf_fd
             * is still pending_fd, ICD didn't take it → close it. */
            if (vk_dmabuf_fd == pending_fd) {
                close(pending_fd);
                vk_dmabuf_fd = -1;
            }
            vkEndCommandBuffer(recording_cmd);
            vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &local_cmd);
            return;
        }
        /* Import succeeded — ICD owns the fd, vk_dmabuf_fd tracks it. */
    } else if (vk_shm_fd >= 0) {
        if (vk_upload_shm(vk_shm_fd, vk_overlay_w, vk_overlay_h,
                          vk_overlay_w * vk_overlay_h * 4, 0, recording_cmd) != 0) {
            IDK_ERR("comp-vk", "render_overlay: SHM upload failed (vk_shm_fd=%d)\n", vk_shm_fd);
            vkEndCommandBuffer(recording_cmd);
            vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &local_cmd);
            return;
        }
    } else {
        IDK_LOG("comp-vk", "render_overlay: no SHM or DMABUF data\n");
    }

    if (vk_overlay_view == VK_NULL_HANDLE) {
        IDK_ERR("comp-vk", "overlay_view NULL — skipping render\n");
        vkEndCommandBuffer(recording_cmd);
        vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &local_cmd);
        return;
    }

    IDK_LOG("comp-vk", "render_overlay: proceeding with render pass\n");

    /* Update descriptor set */
    VK_LOAD(vkUpdateDescriptorSets);
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = vk_desc_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &(VkDescriptorImageInfo){
            .sampler = vk_sampler,
            .imageView = vk_overlay_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
    };
    vkUpdateDescriptorSets(vk_dev, 1, &write, 0, NULL);

    /* Create framebuffer for this swapchain image */
    VK_LOAD(vkCreateFramebuffer);
    VK_LOAD(vkDestroyFramebuffer);
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = swapchainImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = vk_rp_format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView swapchain_view;
    if (vkCreateImageView(vk_dev, &vci, NULL, &swapchain_view) != VK_SUCCESS) {
        vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &local_cmd);
        return;
    }

    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = vk_render_pass,
        .attachmentCount = 1,
        .pAttachments = &swapchain_view,
        .width = width,
        .height = height,
        .layers = 1,
    };
    VkFramebuffer fb;
    if (vkCreateFramebuffer(vk_dev, &fbci, NULL, &fb) != VK_SUCCESS) {
        vkDestroyImageView(vk_dev, swapchain_view, NULL);
        vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &local_cmd);
        return;
    }

    /* Begin render pass */
    VK_LOAD(vkCmdBeginRenderPass);
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = vk_render_pass,
        .framebuffer = fb,
        .renderArea = { {0, 0}, {width, height} },
    };
    vkCmdBeginRenderPass(recording_cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    /* Bind pipeline + draw */
    VK_LOAD(vkCmdBindPipeline);
    vkCmdBindPipeline(recording_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);

    VkViewport vp = { .width = width, .height = height, .minDepth = 0, .maxDepth = 1 };
    VK_LOAD(vkCmdSetViewport);
    vkCmdSetViewport(recording_cmd, 0, 1, &vp);
    VkRect2D sc = { {0, 0}, {width, height} };
    VK_LOAD(vkCmdSetScissor);
    vkCmdSetScissor(recording_cmd, 0, 1, &sc);

    VK_LOAD(vkCmdBindDescriptorSets);
    vkCmdBindDescriptorSets(recording_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pll, 0, 1, &vk_desc_set, 0, NULL);

    VK_LOAD(vkCmdDraw);
    vkCmdDraw(recording_cmd, 3, 1, 0, 0);

    VK_LOAD(vkCmdEndRenderPass);
    vkCmdEndRenderPass(recording_cmd);

    vkEndCommandBuffer(recording_cmd);

    /* Submit + wait (synchronous) */
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    if (vkCreateFence(vk_dev, &fci, NULL, &fence) != VK_SUCCESS) {
        vkDestroyFramebuffer(vk_dev, fb, NULL);
        vkDestroyImageView(vk_dev, swapchain_view, NULL);
        vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &local_cmd);
        return;
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &recording_cmd };
    VkResult r = vkQueueSubmit(vk_queue, 1, &si, fence);
    if (r == VK_SUCCESS) vkWaitForFences(vk_dev, 1, &fence, VK_TRUE, 1000000000ULL);
    else IDK_ERR("comp-vk", "QueueSubmit failed: %d\n", r);
    vkDestroyFence(vk_dev, fence, NULL);
    vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &local_cmd);

    /* Cleanup per-frame objects */
    vkDestroyFramebuffer(vk_dev, fb, NULL);
    vkDestroyImageView(vk_dev, swapchain_view, NULL);
}

/* ── Has overlay ───────────────────────────────────────────────────────── */

int idk_vk_compositor_has_overlay(void) {
    return vk_has_frame;
}

/* ── Init ──────────────────────────────────────────────────────────────── */

int idk_vk_compositor_init(VkDevice device, VkPhysicalDevice physDevice,
                           uint32_t queueFamily,
                           PFN_vkGetDeviceProcAddr gpa,
                           PFN_vkGetInstanceProcAddr instanceGpa) {
    vk_dev = device;
    vk_phys = physDevice;
    vk_queue_family = queueFamily;
    vk_gpa = gpa;

    /* Load instance-level functions via the VkInstance stored during CreateInstance.
     * vkGetPhysicalDeviceMemoryProperties is instance-level — needs a valid VkInstance.
     * We get the instance from vulkan_layer.c (stored during CreateInstance) and use
     * the instanceGpa parameter passed in by the layer (the layer's GetInstanceProcAddr). */
    extern VkInstance idk_vk_layer_get_instance(void);

    VkInstance inst = idk_vk_layer_get_instance();

    if (inst != VK_NULL_HANDLE && instanceGpa) {
        if (!vk_fn_GetPhysMemProps)
            vk_fn_GetPhysMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
                instanceGpa(inst, "vkGetPhysicalDeviceMemoryProperties");
        IDK_LOG("comp-vk", "GetPhysMemProps loaded=%p (instance=%p gpa=%p)\n",
                (void*)vk_fn_GetPhysMemProps, (void*)inst, (void*)instanceGpa);
    } else {
        IDK_ERR("comp-vk", "No VkInstance or GPA available (inst=%p gpa=%p)\n",
                (void*)inst, (void*)instanceGpa);
    }

    /* Get queue */
    VK_LOAD(vkGetDeviceQueue);
    vkGetDeviceQueue(vk_dev, vk_queue_family, 0, &vk_queue);

    /* Query GPU vendor for cross-GPU dmabuf detection.
     * We compare the dmabuf modifier's vendor (bits 56-63) with our GPU's
     * vendor. If mismatch → the dmabuf was produced by a different GPU
     * (e.g. webview on Intel iGPU, compositor on NVIDIA dGPU) → cross-vendor
     * dmabuf import produces garbage → reject, force SHM.
     *
     * IMPORTANT: use the `instanceGpa` PARAMETER (passed by vulkan_layer.c),
     * NOT the `vk_fn_GetInstanceProcAddr` static — that static is only set
     * by comp_vk_set_instance_gpa() which is never called, so it stays NULL
     * and the vendor detection silently no-ops. */
    if (inst != VK_NULL_HANDLE && instanceGpa) {
        PFN_vkGetPhysicalDeviceProperties fnGetPhysDevProps =
            (PFN_vkGetPhysicalDeviceProperties)instanceGpa(inst, "vkGetPhysicalDeviceProperties");
        if (fnGetPhysDevProps) {
            VkPhysicalDeviceProperties props;
            fnGetPhysDevProps(vk_phys, &props);
            vk_vk_vendor_id = props.vendorID;
            vk_drm_vendor_id = idk_vk_vendor_to_drm(props.vendorID);
            IDK_LOG("comp-vk", "GPU vendor: Vk=0x%x → DRM=0x%02x (%s)\n",
                    props.vendorID, vk_drm_vendor_id,
                    vk_drm_vendor_id == 0x03 ? "NVIDIA" :
                    vk_drm_vendor_id == 0x01 ? "Intel" :
                    vk_drm_vendor_id == 0x02 ? "AMD" : "unknown");
        } else {
            IDK_ERR("comp-vk", "vkGetPhysicalDeviceProperties not loaded — cross-GPU detection disabled\n");
        }
    } else {
        IDK_ERR("comp-vk", "No instance/gpa for vendor detection (inst=%p gpa=%p)\n",
                (void*)inst, (void*)instanceGpa);
    }

    if (vk_create_pipeline_objects() != 0) {
        IDK_ERR("comp-vk", "Pipeline init failed\n");
        return -1;
    }

    if (vk_sock_init() != 0) {
        IDK_ERR("comp-vk", "Socket init failed\n");
        return -1;
    }

    IDK_LOG("comp-vk", "Initialized (device=%p phys=%p queue=%p)\n",
            (void*)vk_dev, (void*)vk_phys, (void*)vk_queue);
    return 0;
}

/* ── Shutdown ──────────────────────────────────────────────────────────── */

void idk_vk_compositor_shutdown(void) {
    idk_tp_destroy(&vk_tp);
    if (vk_sock_path[0]) unlink(vk_sock_path);
    /* Vulkan objects destroyed by OS on process exit */
    vk_has_frame = 0;
    IDK_LOG("comp-vk", "Shut down\n");
}

#endif /* IDK_HAVE_VK_LAYER */
