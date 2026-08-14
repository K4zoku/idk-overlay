/* render/vk context.h - shared state for the Vulkan compositor backend */

#ifndef IDK_RENDER_VK_CONTEXT_H
#define IDK_RENDER_VK_CONTEXT_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include <vulkan/vulkan.h>

/* Internal helpers: not part of the public symbol surface. */
#define IDK_INTERNAL __attribute__((visibility("hidden")))

#define VK_ASYNC_RING_SIZE 2
#define VK_SWAPCHAIN_COOLDOWN_MS 100

/* Async submit ring slot: resources deferred for cleanup until the slot's
 * fence signals (the GPU finished ~2 frames ago). */
typedef struct vk_async_slot {
  VkFence fence; /* VK_NULL_HANDLE when slot is free */
  VkCommandBuffer cmd;
  VkFramebuffer fb;
  VkImageView view;
} vk_async_slot_t;

typedef struct {
  VkDevice vk_dev;
  VkPhysicalDevice vk_phys;
  uint32_t vk_queue_family;
  VkQueue vk_queue;
  VkCommandPool vk_cmd_pool;
  uint32_t vk_vk_vendor_id;  /* VkPhysicalDeviceProperties.vendorID */
  uint32_t vk_drm_vendor_id; /* mapped DRM modifier vendor (0=unknown) */
} vk_device_t;

typedef struct {
  PFN_vkGetDeviceProcAddr vk_gpa;
  PFN_vkGetPhysicalDeviceMemoryProperties vk_fn_GetPhysMemProps;
  PFN_vkGetInstanceProcAddr vk_fn_GetInstanceProcAddr;
} vk_gpa_t;

typedef struct {
  VkDescriptorSetLayout vk_dsl;
  VkPipelineLayout vk_pll;
  VkPipeline vk_pipeline;
  VkSampler vk_sampler;
  VkDescriptorPool vk_desc_pool;
  VkDescriptorSet vk_desc_set;
} vk_pipeline_t;

typedef struct {
  VkRenderPass vk_render_pass;
  VkFormat vk_rp_format;
} vk_renderpass_t;

typedef struct {
  VkImage vk_overlay_img;
  VkImageView vk_overlay_view;
  uint32_t vk_overlay_w; /* current frame's width (from webview) */
  uint32_t vk_overlay_h; /* current frame's height (from webview) */
  uint32_t vk_shm_img_w; /* actual allocated VkImage width */
  uint32_t vk_shm_img_h; /* actual allocated VkImage height */
  int vk_has_frame;
} vk_overlay_t;

typedef struct {
  VkBuffer vk_staging_buf;
  VkDeviceMemory vk_staging_mem;
  void *vk_staging_mapped;
  VkDeviceSize vk_staging_size;
  VkImage vk_shm_img;
  VkDeviceMemory vk_shm_img_mem;
  VkImageView vk_shm_view;
  int vk_shm_fd;
} vk_staging_t;

typedef struct {
  int vk_dmabuf_fd;            /* currently-imported dmabuf fd */
  uint16_t vk_dmabuf_cache_id; /* buf_id of cached import (0=none) */
  uint32_t vk_dmabuf_w;
  uint32_t vk_dmabuf_h;
  uint32_t vk_dmabuf_stride;
  uint32_t vk_dmabuf_fourcc;
  uint64_t vk_dmabuf_modifier;
  VkImage vk_dmabuf_img;
  VkDeviceMemory vk_dmabuf_img_mem;
  VkImageView vk_dmabuf_view;
  /* Pending dmabuf - received but not yet imported (import happens in
   * render_overlay because we need a command buffer for the layout
   * transition). */
  int vk_dmabuf_pending_fd;
  uint32_t vk_dmabuf_pending_w;
  uint32_t vk_dmabuf_pending_h;
  uint32_t vk_dmabuf_pending_stride;
  uint32_t vk_dmabuf_pending_fourcc;
  uint64_t vk_dmabuf_pending_modifier;
  uint16_t vk_dmabuf_pending_buf_id;
  int vk_has_dmabuf_pending;
} vk_dmabuf_t;

typedef struct {
  vk_async_slot_t vk_async_ring[VK_ASYNC_RING_SIZE];
  int vk_async_ring_idx;  /* next slot to use */
  int vk_async_ring_init; /* fences created? */
} vk_ring_t;

typedef struct {
  int vk_dmabuf_failed_this_frame;
  struct timespec vk_last_swapchain_create_ts;
  pthread_mutex_t vk_render_lock;
} vk_flags_t;

/* All Vulkan compositor state. Field names keep the original vk_ prefix.
 * Single instance in context.c; accessed via vk_ctx(). */
typedef struct vk_context_t {
  vk_device_t device;
  vk_gpa_t gpa;
  vk_pipeline_t pipeline;
  vk_renderpass_t renderpass;
  vk_overlay_t overlay;
  vk_staging_t staging;
  vk_dmabuf_t dmabuf;
  vk_ring_t ring;
  vk_flags_t flags;
} vk_context_t;

/* Single compositor context instance (context.c). */
IDK_INTERNAL vk_context_t *vk_ctx(void);

/* Helper macro to load device functions via the device gpa. Declares a
 * function-local static pointer, loaded once on first use. */
#define VK_LOAD(name)                                                                                                  \
  static PFN_##name name = NULL;                                                                                       \
  if (!name)                                                                                                           \
  name = (PFN_##name)vk_ctx()->gpa.vk_gpa(vk_ctx()->device.vk_dev, #name)

/* descset.c */
IDK_INTERNAL int vk_create_pipeline_objects(void);
IDK_INTERNAL void descset_destroy(void);

/* renderpass.c */
IDK_INTERNAL int vk_create_render_pass(VkFormat format);
IDK_INTERNAL void renderpass_destroy(void);
IDK_INTERNAL int renderpass_make_framebuffer(VkImage img, uint32_t w, uint32_t h, VkFramebuffer *fb, VkImageView *view);

/* pipeline.c */
IDK_INTERNAL int vk_create_pipeline(VkFormat format);

/* shaders.c */
IDK_INTERNAL int vk_create_shader_modules(VkShaderModule *out_vert, VkShaderModule *out_frag);

/* format.c */
IDK_INTERNAL VkFormat drm_fourcc_to_vk_format(uint32_t fourcc);
IDK_INTERNAL int dmabuf_vendor_check(uint64_t modifier);
IDK_INTERNAL uint32_t dmabuf_bind_memory(int fd, VkImage img, VkDeviceSize *out_size);
IDK_INTERNAL uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits,
                                    VkMemoryPropertyFlags props);

/* upload.c */
IDK_INTERNAL int vk_upload_shm(int fd, uint32_t w, uint32_t h, uint32_t pixel_size, uint32_t buf_idx,
                               VkCommandBuffer cmd);

/* dmabuf.c */
IDK_INTERNAL int vk_upload_dmabuf(int fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t fourcc, uint64_t modifier,
                                  uint16_t buf_id, VkCommandBuffer cmd);

/* cache.c */
IDK_INTERNAL int dmabuf_check_cache(int fd, uint32_t w, uint32_t h, uint32_t stride, uint32_t fourcc, uint64_t modifier,
                                    uint16_t buf_id);
IDK_INTERNAL void dmabuf_destroy(void);
IDK_INTERNAL int overlay_upload(VkCommandBuffer cmd);
IDK_INTERNAL void copy_buf_to_img(VkCommandBuffer cmd, uint32_t w, uint32_t h);
IDK_INTERNAL void dmabuf_barrier(VkCommandBuffer cmd, VkImage img);

/* ring.c */
IDK_INTERNAL int ring_cooldown_active(void);
IDK_INTERNAL VkCommandBuffer ring_begin_cmd(void);
IDK_INTERNAL int ring_submit(VkCommandBuffer cmd, VkFramebuffer fb, VkImageView view);
IDK_INTERNAL void ring_shutdown(void);

#endif
