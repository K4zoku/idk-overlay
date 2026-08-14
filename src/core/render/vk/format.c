/* render/vk format.c - dmabuf format + memory compatibility:
 * fourcc → VkFormat mapping, cross-GPU vendor check, memory type selection */

#include "context.h"
#include "core/compositor.h"
#include "core/log.h"

/* Map DRM fourcc → VkFormat.
 *
 * DRM fourcc convention: fourcc_code(a,b,c,d) = a | (b<<8) | (c<<16) | (d<<24).
 * The 4-char name describes the PIXEL as a 32-bit word: MSB→LSB = first→last char.
 * "ARGB8888" = word [31:0] = A:R:G:B (A in high bits, B in low bits).
 *
 * In little-endian memory, the LOW byte of the word is at the LOW address,
 * so DRM_FORMAT_ARGB8888 (word A:R:G:B) → memory bytes B,G,R,A (low→high).
 * VkFormat naming uses MEMORY byte order: B8G8R8A8 = bytes B,G,R,A in memory.
 * Therefore:
 *   DRM_FORMAT_ARGB8888 (A:R:G:B word) ↔ Vk B8G8R8A8_UNORM (B,G,R,A memory)
 *   DRM_FORMAT_ABGR8888 (A:B:G:R word) ↔ Vk R8G8B8A8_UNORM (R,G,B,A memory)
 *   DRM_FORMAT_RGBA8888 (R:G:B:A word) ↔ Vk A8B8G8R8_UNORM_PACK32 (A,B,G,R memory)
 *   DRM_FORMAT_BGRA8888 (B:G:R:A word) → memory B,G,R,A → B8G8R8A8_UNORM (Vulkan
 *     has no A8R8G8B8_UNORM_PACK32, but B8G8R8A8 has the same memory layout). */
VkFormat drm_fourcc_to_vk_format(uint32_t fourcc) {
  if (fourcc == 0x34324241u)
    return VK_FORMAT_R8G8B8A8_UNORM;
  if (fourcc == 0x34325241u)
    return VK_FORMAT_B8G8R8A8_UNORM;
  if (fourcc == 0x34324258u)
    return VK_FORMAT_R8G8B8A8_UNORM;
  if (fourcc == 0x34325258u)
    return VK_FORMAT_B8G8R8A8_UNORM;
  if (fourcc == 0x34324152u)
    return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
  if (fourcc == 0x34324142u)
    return VK_FORMAT_B8G8R8A8_UNORM;
  return VK_FORMAT_R8G8B8A8_UNORM;
}

/* First memory type allowed by `bits` whose propertyFlags include all of
 * `props`, or 0xFFFFFFFF if none. */
uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags props) {
  for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
    if ((bits & (1 << i)) && (mp->memoryTypes[i].propertyFlags & props))
      return i;
  }
  return 0xFFFFFFFF;
}

/* Cross-GPU vendor check for dmabuf import. If the dmabuf modifier's vendor
 * doesn't match our GPU vendor, reject the import and force the SHM fallback.
 * Modifier=0 (linear) or INVALID bypasses this check. */
int dmabuf_vendor_check(uint64_t modifier) {
  vk_context_t *ctx = vk_ctx();
  if (ctx->device.vk_drm_vendor_id != 0 && modifier != 0 && modifier != IDK_DRM_FORMAT_MOD_INVALID) {
    uint32_t mod_vendor = IDK_DRM_MOD_VENDOR(modifier);
    if (mod_vendor != ctx->device.vk_drm_vendor_id) {
      IDK_LOG("comp-vk",
              "dmabuf: cross-GPU vendor mismatch (modifier vendor=0x%02x, our vendor=0x%02x) - rejecting, force SHM\n",
              mod_vendor, ctx->device.vk_drm_vendor_id);
      ctx->flags.vk_dmabuf_failed_this_frame = 1;
      return -1;
    }
  }
  return 0;
}

/* Pick a compatible memory type for the dmabuf import: intersect the image's
 * allowed types with the fd's allowed types (via vkGetMemoryFdPropertiesKHR),
 * preferring DEVICE_LOCAL. Returns the memory type index, or 0xFFFFFFFF on
 * failure (the image is destroyed). *out_size receives the allocation size. */
uint32_t dmabuf_bind_memory(int fd, VkImage img, VkDeviceSize *out_size) {
  vk_context_t *ctx = vk_ctx();
  VkResult r;
  VK_LOAD(vkGetImageMemoryRequirements);
  VK_LOAD(vkGetImageMemoryRequirements2);
  VK_LOAD(vkGetMemoryFdPropertiesKHR);
  VK_LOAD(vkDestroyImage);

  VkMemoryRequirements2 mr2 = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
  };
  VkImageMemoryRequirementsInfo2 mr_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = img,
  };
  if (vkGetImageMemoryRequirements2) {
    vkGetImageMemoryRequirements2(ctx->device.vk_dev, &mr_info, &mr2);
  } else {
    vkGetImageMemoryRequirements(ctx->device.vk_dev, img, &mr2.memoryRequirements);
  }

  VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      .memoryTypeBits = 0,
  };
  r = vkGetMemoryFdPropertiesKHR(ctx->device.vk_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props);
  if (r != VK_SUCCESS) {
    IDK_ERR("comp-vk", "dmabuf: GetMemoryFdPropertiesKHR failed: %d - frame rejected\n", r);
    vkDestroyImage(ctx->device.vk_dev, img, NULL);
    ctx->dmabuf.vk_dmabuf_img = VK_NULL_HANDLE;
    ctx->flags.vk_dmabuf_failed_this_frame = 1;
    return 0xFFFFFFFF;
  }

  if (!ctx->gpa.vk_fn_GetPhysMemProps) {
    IDK_ERR("comp-vk", "dmabuf: vk_fn_GetPhysMemProps not loaded\n");
    vkDestroyImage(ctx->device.vk_dev, img, NULL);
    ctx->dmabuf.vk_dmabuf_img = VK_NULL_HANDLE;
    return 0xFFFFFFFF;
  }
  VkPhysicalDeviceMemoryProperties mp;
  ctx->gpa.vk_fn_GetPhysMemProps(ctx->device.vk_phys, &mp);

  uint32_t allowed = mr2.memoryRequirements.memoryTypeBits & fd_props.memoryTypeBits;
  uint32_t mem_type = 0xFFFFFFFF;
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
    if ((allowed & (1 << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      mem_type = i;
      break;
    }
  }
  if (mem_type == 0xFFFFFFFF) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if (allowed & (1 << i)) {
        mem_type = i;
        break;
      }
    }
  }
  if (mem_type == 0xFFFFFFFF) {
    IDK_ERR("comp-vk", "dmabuf: no compatible mem type (image=0x%x fd=0x%x) - frame rejected\n",
            mr2.memoryRequirements.memoryTypeBits, fd_props.memoryTypeBits);
    vkDestroyImage(ctx->device.vk_dev, img, NULL);
    ctx->dmabuf.vk_dmabuf_img = VK_NULL_HANDLE;
    ctx->flags.vk_dmabuf_failed_this_frame = 1;
    return 0xFFFFFFFF;
  }
  *out_size = mr2.memoryRequirements.size;
  return mem_type;
}
