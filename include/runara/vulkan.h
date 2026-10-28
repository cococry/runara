#pragma once

#include "runara/runara.h"

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

enum rn_vk_target_mode_t {
  /* Managed: Runara creates a WSI surface, swapchain and presents each frame */
  RN_VK_TARGET_MANAGED = 0,
  /* External: the host owns images and Runara records and submits rendering */
  RN_VK_TARGET_EXTERNAL
};

typedef uint32_t rn_vk_target_handle_t;

#define RN_VK_INVALID_TARGET UINT32_MAX

/* Window callbacks for creating and sizing a native VkSurfaceKHR. */
struct rn_vk_surface_impl_t {
  /* Per-window user data passed to both callbacks. */
  void *user;

  /* Creates a native VkSurfaceKHR. Runara owns and destroys this handle. */
  VkResult (*create_vk_surface)(void *user, VkInstance instance,
                                VkSurfaceKHR *out_vk_surface);

  /* Returns the window framebuffer dimensions in physical pixels. */
  void (*framebuffer_size)(void *user, uint32_t *width, uint32_t *height);
};

/* Specifies options for Vulkan initialization */
struct rn_vk_init_options_t {
  /* Selects managed window presentation or host-owned external images. */
  enum rn_vk_target_mode_t mode;

  /* User data passed to instance_extensions and accept_device. */
  void *user;

  /* Optional in external mode. Return required instance extensions.
   * The returned names must remain valid for vkCreateInstance().
   */
  const char *const *(*instance_extensions)(void *user, uint32_t *count);

  /* Optional in all modes. */
  bool (*accept_device)(void *user, VkInstance instance,
                        VkPhysicalDevice physical_device);

  /* Optional additional device extensions provided by the user */
  const char *const *device_extensions;
  uint32_t device_extension_count;

  /* External mode uses this format for its compatible render pass.
   * VK_FORMAT_UNDEFINED selects VK_FORMAT_B8G8R8A8_UNORM.
   * All registered external targets must use the selected format.
   */
  VkFormat external_format;
};

/* Describes all Vulkan native handles managed by Runara */
struct rn_vk_native_handles_t {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue graphics_queue;
  uint32_t graphics_queue_family;
};

/* Borrow a host created VkImage
 * The image and its VkDeviceMemory remain owned by the host.
 * Runara owns only its image view and framebuffer.
 *
 * Images must have VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT and be compatible
 * with the selected format. The host must not destroy an image while it is
 * registered. The host must ensure any external uses finished before
 * selecting it for another frame.
 */
struct rn_vk_external_target_desc_t {
  VkImage image;
  VkFormat format;
  uint32_t width;
  uint32_t height;
};

struct rn_state_t;

/* Opaque Runara object owning a native VkSurfaceKHR and its swapchain. */
struct rn_vk_managed_surface_t;

/* Creates a managed window target and selects it for rendering.
 * Targets share VK_FORMAT_B8G8R8A8_UNORM. Runara destroys remaining targets
 * at shutdown. Callback user data must remain valid until target destruction.
 * A zero-sized window gets its swapchain when its framebuffer becomes nonzero.
 */
rn_result_t
rn_vk_surface_create(struct rn_state_t *state,
                     const struct rn_vk_surface_impl_t *impl,
                     struct rn_vk_managed_surface_t **out_managed_surface);

/* Requires a Vulkan state in managed mode. Selects a Runara managed target (a
 * rn_vk_managed_surface_t) to render to. Call this before rn_begin().
 * This only assigns a pointer. NULL deselects the target.
 */
void rn_vk_surface_set_current(struct rn_state_t *state,
                               struct rn_vk_managed_surface_t *managed_surface);

/* Destroys the Runara target, its swapchain and its native VkSurfaceKHR.
 * Call between frames, before destroying the native window.
 */
rn_result_t rn_vk_surface_destroy(
    struct rn_state_t *state, struct rn_vk_managed_surface_t *managed_surface);

/* Returns all Vulkan native handles managed by Runara */
rn_result_t
rn_vk_get_native_handles(struct rn_state_t *state,
                         struct rn_vk_native_handles_t *out_handles);

/* Registers an external render target for Runara. Vulkan must have been
 * initialized with .mode = RN_VK_TARGET_EXTERNAL for this.*/
rn_result_t
rn_vk_target_register(struct rn_state_t *state,
                      const struct rn_vk_external_target_desc_t *desc,
                      rn_vk_target_handle_t *out_target);

/* Sets the current external Vulkan render target. Vulkan must have been
 * initialized with .mode = RN_VK_TARGET_EXTERNAL for this.*/
rn_result_t rn_vk_target_set(struct rn_state_t *state,
                             rn_vk_target_handle_t target);

/* Unregisters an external render target for Runara. Vulkan must have been
 * initialized with .mode = RN_VK_TARGET_EXTERNAL for this.*/
rn_result_t rn_vk_target_unregister(struct rn_state_t *state,
                                    rn_vk_target_handle_t target);

/* Wait for the most recent Runara rendering submission to finish.
 * This is a correct but blocking synchronization path. */
rn_result_t rn_vk_wait_for_render(struct rn_state_t *state);

/* Borrow a previously imported sampled VkImage.
 * Runara creates/destroys its view and sampler, but not the VkImage/memory.
 * The caller must make the image SHADER_READ_ONLY_OPTIMAL and provide the
 * required acquire synchronization before drawing it.
 * rn_texture_release() releases the Runara wrapper, not the borrowed image.
 */
rn_result_t rn_image_from_vk_image(struct rn_state_t *state, VkImage image,
                                   VkFormat format, uint32_t width,
                                   uint32_t height,
                                   struct rn_image_t *out_image);
