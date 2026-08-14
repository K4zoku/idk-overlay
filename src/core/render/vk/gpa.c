/* render/vk gpa.c - instance/device GPA dispatch setup */

#include "context.h"
#include "core/compositor_vk.h"

/* Instance-level functions loaded once from the layer's GetInstanceProcAddr.
 * The vk_fn_* pointers live in vk_context_t so upload.c/dmabuf.c/context.c
 * can use them. */
void comp_vk_set_instance_gpa(PFN_vkGetInstanceProcAddr gpa) {
  vk_context_t *ctx = vk_ctx();
  ctx->gpa.vk_fn_GetInstanceProcAddr = gpa;
  if (gpa && !ctx->gpa.vk_fn_GetPhysMemProps)
    ctx->gpa.vk_fn_GetPhysMemProps =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gpa(NULL, "vkGetPhysicalDeviceMemoryProperties");
}
