/*
 * compositor_vk.h - Vulkan-native compositor API
 *
 * Pure Vulkan rendering pipeline - no GL/EGL dependency.
 * Receives overlay frames (SHM or DMABUF) from webview via socket,
 * imports to VkImage, renders overlay on swapchain image via
 * VkPipeline (SPIR-V shaders + alpha blending).
 */

#ifndef IDK_COMPOSITOR_VK_H
#define IDK_COMPOSITOR_VK_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the Vulkan compositor.
 * Must be called after vkCreateDevice, with a valid VkDevice + dispatch table.
 * Creates: descriptor set layout, pipeline layout, sampler, descriptor pool,
 * command pool, query pool. */
int idk_vk_compositor_init(VkDevice device, VkPhysicalDevice physDevice,
                           uint32_t queueFamily,
                           PFN_vkGetDeviceProcAddr gpa,
                           PFN_vkGetInstanceProcAddr instanceGpa);

/* Set instance GPA for loading instance-level functions (e.g. vkGetPhysicalDeviceMemoryProperties) */
void comp_vk_set_instance_gpa(PFN_vkGetInstanceProcAddr gpa);

/* Receive a new overlay frame from webview socket (non-blocking).
 * Returns 0 if frame received, -1 if no frame. */
int idk_vk_compositor_render(void);

/* Render the overlay on the swapchain image.
 * Called from vkQueuePresentKHR before forwarding to the real present.
 * Records commands into the provided command buffer:
 *   - Import/Upload overlay texture
 *   - Begin render pass on swapchain image
 *   - Draw fullscreen triangle
 *   - End render pass
 *
 * The caller must submit the command buffer with appropriate semaphores
 * before calling the real vkQueuePresentKHR. */
void idk_vk_compositor_render_overlay(VkCommandBuffer cmd, VkImage swapchainImage,
                                      uint32_t width, uint32_t height,
                                      VkFormat swapchainFormat);

/* Check if compositor has a frame to render. */
int idk_vk_compositor_has_overlay(void);

/* Notify resize (same as GL compositor - embedded in ACK). */
void idk_vk_compositor_notify_resize(int w, int h);

/* Notify that a swapchain was created (used to skip overlay during storms). */
void idk_vk_compositor_notify_swapchain_created(void);

/* Shut down - destroy Vulkan resources. */
void idk_vk_compositor_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IDK_COMPOSITOR_VK_H */
