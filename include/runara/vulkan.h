#pragma once

#include "runara/runara.h"
#include <stdint.h>
#include <vulkan/vulkan.h>

/* Vulkan device and surface lifetime is owned by the caller.
 * Runara uses these handles but does not own them and never destroys them.
 */
struct rn_vk_device_info_t {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue graphics_queue;
  uint32_t graphics_queue_family;
  VkRenderPass render_pass;
};

struct rn_state_t;

/* Call after beginning a host-owned command buffer and render pass, and before
 * rn_begin() */
rn_result_t rn_vk_set_command_buffer(struct rn_state_t *state,
                                     VkCommandBuffer command_buffer);
