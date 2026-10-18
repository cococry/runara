#include "src/backend.h"
#include "runara/runara.h"
#include "runara/vulkan.h"
#include "src/internal.h"
#include "src/log.h"
#include "src/vulkan/shaders.h"

#define VMA_VULKAN_VERSION 1000000
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vk_mem_alloc.h>

#define RN_SUBSYS_NAME "VULKAN"
#define RN_VK_MAX_BINDINGS 8u
#define RN_VK_MAX_TEXTURES 16u
#define RN_VK_DESCRIPTOR_PAGE_SIZE 128u

#define _VK_TRACE(log, ...)                                                    \
  do {                                                                         \
    if ((log) != NULL)                                                         \
      RN_TRACE(*(log), __VA_ARGS__);                                           \
  } while (0)

#define _VK_ERROR(log, ...)                                                    \
  do {                                                                         \
    if ((log) != NULL)                                                         \
      RN_ERROR(*(log), __VA_ARGS__);                                           \
    else                                                                       \
      RN_ERROR_HEADLESS(__VA_ARGS__);                                          \
  } while (0)

struct rn_vk_buffer {
  VkBuffer buffer;
  VmaAllocation allocation;
  VkDeviceSize size;
  enum rn_buffer_target_t target;
  bool alive;
};

struct rn_vk_texture {
  VkImage image;
  VkImageView view;
  VkSampler sampler;
  VmaAllocation allocation;
  uint32_t width, height, layers;
  bool array;
  bool borrowed;
  bool alive;
};

struct rn_vk_descriptor_page {
  VkDescriptorPool pool;
  uint32_t used;
  struct rn_vk_descriptor_page *next;
};

struct rn_vk_descriptor_entry {
  VkDescriptorSet set;
  rn_texture_handle_t textures[RN_VK_MAX_TEXTURES];
  uint32_t count;
  struct rn_vk_descriptor_entry *next;
};

struct rn_vk_pipeline {
  VkPipeline pipeline;
  VkPipelineLayout layout;
  VkDescriptorSetLayout set_layout;
  struct rn_vk_descriptor_page *pages;
  struct rn_vk_descriptor_entry *descriptors;
  enum rn_shader_pipeline_t shader;
  uint32_t descriptor_count;
  uint32_t strides[RN_VK_MAX_BINDINGS];
  bool alive;
};

struct rn_vk_external_target {
  VkImage image;
  VkImageView view;
  VkFramebuffer framebuffer;
  VkExtent2D extent;
  bool alive;
  bool used;
};

struct rn_vk_swap_image {
  VkImageView view;
  VkFramebuffer framebuffer;
  VkSemaphore render_done;
};

struct rn_vk_context {
  enum rn_vk_target_mode_t mode;
  struct rn_vk_init_options_t config;
  struct rn_vk_external_target *external_targets;
  size_t external_target_count;
  rn_vk_target_handle_t active_target;
  VkExtent2D frame_extent;
  bool frame_submitted;

  VkInstance instance;
  VkSurfaceKHR surface;
  VkSwapchainKHR swapchain;
  VkQueue present_queue;
  uint32_t present_family;
  VkFormat swap_format;
  VkExtent2D swap_extent;
  struct rn_vk_swap_image *swap_images;
  uint32_t swap_image_count;
  uint32_t image_index;
  uint32_t requested_width, requested_height;
  bool resize_pending;
  bool fatal;
  bool tearing_down;

  VkCommandPool frame_pool;
  VkCommandBuffer frame_command;
  VkSemaphore image_available;
  VkFence frame_fence;

  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family;
  VkRenderPass render_pass;
  VkCommandBuffer command_buffer;
  VkCommandPool upload_pool;
  VkCommandBuffer upload_command;
  VkFence upload_fence;
  VmaAllocator vma;
  VkPhysicalDeviceProperties properties;

  struct rn_vk_buffer *buffers;
  size_t buffer_count;
  struct rn_vk_texture *textures;
  size_t texture_count;
  struct rn_vk_pipeline *pipelines;
  size_t pipeline_count;

  rn_pipeline_handle_t pipeline;
  rn_buffer_handle_t vertex_buffers[RN_VK_MAX_BINDINGS];
  rn_buffer_handle_t index_buffer;
  rn_texture_handle_t bound_textures[RN_VK_MAX_TEXTURES];
  uint32_t extent_x, extent_y, extent_width, extent_height;
  bool frame_active;

  const struct rn_allocator_t *alloc;
  const struct rn_log_t *log;
  struct rn_vk_platform_t platform;
};

static rn_result_t _result(struct rn_vk_context *ctx, VkResult status,
                           const char *op);
static rn_result_t _create_instance(struct rn_vk_context *ctx);
static rn_result_t _create_device(struct rn_vk_context *ctx);
static rn_result_t _create_render_pass(struct rn_vk_context *ctx,
                                       VkFormat format);
static struct rn_vk_external_target *
_external_target(struct rn_vk_context *ctx, rn_vk_target_handle_t handle);
static void _destroy_external_targets(struct rn_vk_context *ctx);
static rn_result_t _create_swapchain(struct rn_vk_context *ctx, uint32_t width,
                                     uint32_t height);
static void _destroy_swapchain(struct rn_vk_context *ctx);
static rn_result_t _create_frame_commands(struct rn_vk_context *ctx);
static void *_grow(struct rn_vk_context *ctx, void *array, size_t count,
                   size_t item_size);
static struct rn_vk_buffer *_buffer(struct rn_vk_context *ctx,
                                    rn_buffer_handle_t handle);
static struct rn_vk_texture *_texture(struct rn_vk_context *ctx,
                                      rn_texture_handle_t handle);
static struct rn_vk_pipeline *_pipeline(struct rn_vk_context *ctx,
                                        rn_pipeline_handle_t handle);
static rn_result_t _begin_frame(void *backend_ctx, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height);
static rn_result_t _clear_color(void *backend_ctx, rn_color_t color);
static rn_result_t _end_frame(void *backend_ctx);
static rn_result_t _create_buffer(void *backend_ctx,
                                  const struct rn_buffer_desc_t *desc,
                                  rn_buffer_handle_t *out);
static void _destroy_buffer(void *backend_ctx, rn_buffer_handle_t handle);
static rn_result_t _write_buffer(void *backend_ctx, rn_buffer_handle_t handle,
                                 uint32_t binding, size_t offset,
                                 const void *data, size_t size);
static rn_result_t _create_pipeline(void *backend_ctx,
                                    const struct rn_pipeline_desc_t *desc,
                                    rn_pipeline_handle_t *out);
static void _destroy_pipeline(void *backend_ctx, rn_pipeline_handle_t handle);
static rn_result_t _set_pipeline(void *backend_ctx,
                                 rn_pipeline_handle_t handle);
static rn_result_t _set_vertex_buffer(void *backend_ctx,
                                      rn_buffer_handle_t handle,
                                      uint32_t binding);
static rn_result_t _set_index_buffer(void *backend_ctx,
                                     rn_buffer_handle_t handle);
static rn_result_t _set_uniform_2f(void *backend_ctx,
                                   rn_pipeline_handle_t handle,
                                   const char *name, float x, float y);
static rn_result_t
_draw_indexed_instanced(void *backend_ctx,
                        const struct rn_draw_indexed_instanced_t *draw);
static rn_result_t _create_texture(void *backend_ctx,
                                   const struct rn_texture_desc_t *desc,
                                   const void *pixels, size_t size,
                                   rn_texture_handle_t *out);
static rn_result_t
_create_texture_array(void *backend_ctx,
                      const struct rn_texture_array_desc_t *desc,
                      rn_texture_handle_t *out);
static rn_result_t
_write_texture_array_layer(void *backend_ctx,
                           const struct rn_texture_array_write_t *write,
                           const void *pixels, size_t size);
static void _destroy_texture(void *backend_ctx, rn_texture_handle_t handle);
static rn_result_t _set_texture(void *backend_ctx, uint32_t slot,
                                rn_texture_handle_t handle);
static rn_result_t _begin_upload(struct rn_vk_context *ctx);
static rn_result_t _end_upload(struct rn_vk_context *ctx);
static void _image_barrier(struct rn_vk_context *ctx, VkImage image,
                           VkImageLayout old_layout, VkImageLayout new_layout,
                           uint32_t layers);
static rn_result_t _upload_image(struct rn_vk_context *ctx,
                                 struct rn_vk_texture *texture, uint32_t layer,
                                 uint32_t x, uint32_t y, uint32_t width,
                                 uint32_t height, const void *pixels,
                                 size_t size);
static rn_result_t _create_texture_internal(struct rn_vk_context *ctx,
                                            uint32_t width, uint32_t height,
                                            uint32_t layers, bool array,
                                            const void *pixels, size_t size,
                                            rn_texture_handle_t *out);
static rn_result_t _descriptor_for_draw(struct rn_vk_context *ctx,
                                        struct rn_vk_pipeline *pipeline,
                                        VkDescriptorSet *out);

static const struct rn_backend_impl_t _rn_vk_dispatch = {
    .begin_frame = _begin_frame,
    .clear_color = _clear_color,
    .end_frame = _end_frame,
    .create_buffer = _create_buffer,
    .destroy_buffer = _destroy_buffer,
    .write_buffer = _write_buffer,
    .create_pipeline = _create_pipeline,
    .destroy_pipeline = _destroy_pipeline,
    .set_pipeline = _set_pipeline,
    .set_vertex_buffer = _set_vertex_buffer,
    .set_index_buffer = _set_index_buffer,
    .set_uniform_2f = _set_uniform_2f,
    .draw_indexed_instanced = _draw_indexed_instanced,
    .create_texture = _create_texture,
    .create_texture_array = _create_texture_array,
    .write_texture_array_layer = _write_texture_array_layer,
    .destroy_texture = _destroy_texture,
    .set_texture = _set_texture,
};

static rn_result_t _result(struct rn_vk_context *ctx, VkResult status,
                           const char *op) {
  if (status == VK_SUCCESS)
    return RN_OK;

  _VK_ERROR(ctx ? ctx->log : NULL, "%s failed (VkResult=%d)", op, (int)status);
  return status == VK_ERROR_OUT_OF_HOST_MEMORY ||
                 status == VK_ERROR_OUT_OF_DEVICE_MEMORY
             ? RN_ERROR_OUT_OF_MEMORY
             : RN_ERROR_BACKEND;
}

static void *_grow(struct rn_vk_context *ctx, void *array, size_t count,
                   size_t item_size) {
  if (count >= UINT32_MAX || count >= SIZE_MAX / item_size)
    return NULL;
  return ctx->alloc->realloc(array, (count + 1) * item_size, ctx->alloc->ctx);
}

static struct rn_vk_buffer *_buffer(struct rn_vk_context *ctx,
                                    rn_buffer_handle_t handle) {
  return handle < ctx->buffer_count && ctx->buffers[handle].alive
             ? &ctx->buffers[handle]
             : NULL;
}

static struct rn_vk_texture *_texture(struct rn_vk_context *ctx,
                                      rn_texture_handle_t handle) {
  return handle < ctx->texture_count && ctx->textures[handle].alive
             ? &ctx->textures[handle]
             : NULL;
}

static struct rn_vk_pipeline *_pipeline(struct rn_vk_context *ctx,
                                        rn_pipeline_handle_t handle) {
  return handle < ctx->pipeline_count && ctx->pipelines[handle].alive
             ? &ctx->pipelines[handle]
             : NULL;
}

static rn_result_t _create_instance(struct rn_vk_context *ctx) {
  const struct rn_vk_platform_t *platform = &ctx->platform;
  uint32_t count = 0;
  const char *const *required = NULL;

  if (platform->instance_extensions)
    required = platform->instance_extensions(platform->user, &count);

  if (count > 30 || (count && !required))
    return RN_ERROR_INVALID_ARGUMENT;

  const char *extensions[32] = {0};
  uint32_t extension_count = 0;

  if (ctx->mode == RN_VK_TARGET_MANAGED)
    extensions[extension_count++] = VK_KHR_SURFACE_EXTENSION_NAME;

  for (uint32_t i = 0; i < count; ++i) {
    if (!required[i])
      return RN_ERROR_INVALID_ARGUMENT;

    bool found = false;
    for (uint32_t j = 0; j < extension_count; ++j) {
      if (strcmp(required[i], extensions[j]) == 0) {
        found = true;
        break;
      }
    }

    if (!found)
      extensions[extension_count++] = required[i];
  }

  const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "Runara",
      .applicationVersion = VK_MAKE_VERSION(2, 0, 0),
      .pEngineName = "Runara",
      .engineVersion = VK_MAKE_VERSION(2, 0, 0),
      .apiVersion = VK_API_VERSION_1_0,
  };

  const VkInstanceCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = extension_count,
      .ppEnabledExtensionNames = extension_count ? extensions : NULL,
  };

  return _result(ctx, vkCreateInstance(&info, NULL, &ctx->instance),
                 "vkCreateInstance");
}

static bool _has_device_extensions(struct rn_vk_context *ctx,
                                   VkPhysicalDevice gpu,
                                   const char *const *required,
                                   uint32_t required_count) {
  if (!required_count)
    return true;

  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL) !=
      VK_SUCCESS)
    return false;

  if (!count)
    return false;

  VkExtensionProperties *available =
      ctx->alloc->alloc((size_t)count * sizeof(*available), ctx->alloc->ctx);

  if (!available)
    return false;

  bool supported = false;
  if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, available) ==
      VK_SUCCESS) {
    supported = true;

    for (uint32_t i = 0; i < required_count; ++i) {
      bool found = false;

      for (uint32_t j = 0; j < count; ++j) {
        if (strcmp(required[i], available[j].extensionName) == 0) {
          found = true;
          break;
        }
      }

      if (!found) {
        supported = false;
        break;
      }
    }
  }

  ctx->alloc->free(available, ctx->alloc->ctx);
  return supported;
}

static rn_result_t _create_device(struct rn_vk_context *ctx) {
  /* creates logical device */

  const char *extensions[32] = {0};
  uint32_t extension_count = 0;

  if (ctx->mode == RN_VK_TARGET_MANAGED)
    extensions[extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  else
    extensions[extension_count++] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;

  if (ctx->config.device_extension_count > 32 - extension_count ||
      (ctx->config.device_extension_count && !ctx->config.device_extensions))
    return RN_ERROR_INVALID_ARGUMENT;

  for (uint32_t i = 0; i < ctx->config.device_extension_count; ++i) {
    const char *name = ctx->config.device_extensions[i];
    if (!name)
      return RN_ERROR_INVALID_ARGUMENT;

    bool found = false;
    for (uint32_t j = 0; j < extension_count; ++j) {
      if (strcmp(extensions[j], name) == 0) {
        found = true;
        break;
      }
    }

    if (!found)
      extensions[extension_count++] = name;
  }

  uint32_t count = 0;
  VkResult status = vkEnumeratePhysicalDevices(ctx->instance, &count, NULL);
  if (status != VK_SUCCESS || !count)
    return _result(
        ctx, status == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : status,
        "vkEnumeratePhysicalDevices");

  VkPhysicalDevice *devices =
      ctx->alloc->alloc((size_t)count * sizeof(*devices), ctx->alloc->ctx);

  if (!devices)
    return RN_ERROR_OUT_OF_MEMORY;

  status = vkEnumeratePhysicalDevices(ctx->instance, &count, devices);
  if (status != VK_SUCCESS) {
    ctx->alloc->free(devices, ctx->alloc->ctx);
    return _result(ctx, status, "vkEnumeratePhysicalDevices");
  }

  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDevice gpu = devices[i];

    if (ctx->platform.accept_device &&
        !ctx->platform.accept_device(ctx->platform.user, ctx->instance, gpu))
      continue;

    if (!_has_device_extensions(ctx, gpu, extensions, extension_count))
      continue;

    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(gpu, &props);

    /* check basic requiremnets for the card */
    if (VK_VERSION_MAJOR(props.apiVersion) < 1 ||
        props.limits.maxPerStageDescriptorSamplers < RN_VK_MAX_TEXTURES ||
        props.limits.maxPerStageDescriptorSampledImages < RN_VK_MAX_TEXTURES)
      continue;

    VkFormat format = ctx->mode == RN_VK_TARGET_EXTERNAL
                          ? ctx->config.external_format
                          : VK_FORMAT_B8G8R8A8_UNORM;

    if (format == VK_FORMAT_UNDEFINED)
      format = VK_FORMAT_B8G8R8A8_UNORM;

    /* if the format does not tile perfectly for color attachments on this card,
     * go to the next one */
    if (ctx->mode == RN_VK_TARGET_EXTERNAL) {
      VkFormatProperties format_props = {0};
      vkGetPhysicalDeviceFormatProperties(gpu, format, &format_props);
      if (!(format_props.optimalTilingFeatures &
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        continue;
    }

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, NULL);
    if (!queue_count)
      continue;

    VkQueueFamilyProperties *queues = ctx->alloc->alloc(
        (size_t)queue_count * sizeof(*queues), ctx->alloc->ctx);

    if (!queues) {
      ctx->alloc->free(devices, ctx->alloc->ctx);
      return RN_ERROR_OUT_OF_MEMORY;
    }

    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, queues);

    uint32_t graphics = UINT32_MAX;
    uint32_t present = UINT32_MAX;

    /* choose presentation and graphics queues */
    for (uint32_t j = 0; j < queue_count; ++j) {
      if (!queues[j].queueCount)
        continue;

      bool can_draw = (queues[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
      VkBool32 can_present = VK_FALSE;

      if (ctx->mode == RN_VK_TARGET_MANAGED) {
        status = vkGetPhysicalDeviceSurfaceSupportKHR(gpu, j, ctx->surface,
                                                      &can_present);

        if (status != VK_SUCCESS)
          continue;
      }

      if (can_draw && (ctx->mode == RN_VK_TARGET_EXTERNAL || can_present)) {
        graphics = j;
        present = j;
        break;
      }

      if (can_draw && graphics == UINT32_MAX)
        graphics = j;

      if (can_present && present == UINT32_MAX)
        present = j;
    }

    ctx->alloc->free(queues, ctx->alloc->ctx);

    if (graphics == UINT32_MAX ||
        (ctx->mode == RN_VK_TARGET_MANAGED && present == UINT32_MAX))
      continue;

    ctx->physical_device = gpu;
    ctx->queue_family = graphics;
    ctx->present_family =
        ctx->mode == RN_VK_TARGET_MANAGED ? present : graphics;
    ctx->properties = props;
    break;
  }

  ctx->alloc->free(devices, ctx->alloc->ctx);

  if (!ctx->physical_device) {
    _VK_ERROR(ctx->log, "no compatible Vulkan 1.0 physical device");
    return RN_ERROR_BACKEND;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queues[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
          .queueFamilyIndex = ctx->queue_family,
          .queueCount = 1,
          .pQueuePriorities = &priority,
      },
      {
          .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
          .queueFamilyIndex = ctx->present_family,
          .queueCount = 1,
          .pQueuePriorities = &priority,
      },
  };

  const VkDeviceCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount =
          ctx->queue_family == ctx->present_family ? 1u : 2u,
      .pQueueCreateInfos = queues,
      .enabledExtensionCount = extension_count,
      .ppEnabledExtensionNames = extensions,
  };

  rn_result_t result = _result(
      ctx, vkCreateDevice(ctx->physical_device, &info, NULL, &ctx->device),
      "vkCreateDevice");

  if (result != RN_OK)
    return result;

  vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);
  vkGetDeviceQueue(ctx->device, ctx->present_family, 0, &ctx->present_queue);

  _VK_TRACE(ctx->log, "created Vulkan 1.0 device %s (graphics=%u, present=%u)",
            ctx->properties.deviceName, ctx->queue_family, ctx->present_family);

  return RN_OK;
}

static rn_result_t _create_render_pass(struct rn_vk_context *ctx,
                                       VkFormat format) {
  /* simply creates a render pass with the specified format.
   * VK_IMAGE_LAYOUT_PRESENT_SRC_KHR is used for managed mode. */
  const VkImageLayout final_layout = ctx->mode == RN_VK_TARGET_MANAGED
                                         ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                         : VK_IMAGE_LAYOUT_GENERAL;

  const VkAttachmentDescription attachment = {
      .format = format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = ctx->mode == RN_VK_TARGET_MANAGED
                           ? VK_IMAGE_LAYOUT_UNDEFINED
                           : VK_IMAGE_LAYOUT_GENERAL,
      .finalLayout = final_layout,
  };

  const VkAttachmentReference color = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };

  const VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color,
  };

  const VkSubpassDependency dependency = {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
  };

  const VkRenderPassCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dependency,
  };

  return _result(
      ctx, vkCreateRenderPass(ctx->device, &info, NULL, &ctx->render_pass),
      "vkCreateRenderPass");
}

static struct rn_vk_external_target *
_external_target(struct rn_vk_context *ctx, rn_vk_target_handle_t handle) {
  if (handle >= ctx->external_target_count)
    return NULL;

  return ctx->external_targets[handle].alive ? &ctx->external_targets[handle]
                                             : NULL;
}

static void _destroy_external_targets(struct rn_vk_context *ctx) {
  for (size_t i = 0; i < ctx->external_target_count; ++i) {
    struct rn_vk_external_target *target = &ctx->external_targets[i];

    if (target->framebuffer)
      vkDestroyFramebuffer(ctx->device, target->framebuffer, NULL);

    if (target->view)
      vkDestroyImageView(ctx->device, target->view, NULL);
  }

  if (ctx->external_targets)
    ctx->alloc->free(ctx->external_targets, ctx->alloc->ctx);

  ctx->external_targets = NULL;
  ctx->external_target_count = 0;
  ctx->active_target = RN_VK_INVALID_TARGET;
}

static void _destroy_swapchain(struct rn_vk_context *ctx) {
  for (uint32_t i = 0; i < ctx->swap_image_count; ++i) {
    struct rn_vk_swap_image *image = &ctx->swap_images[i];
    if (image->render_done)
      vkDestroySemaphore(ctx->device, image->render_done, NULL);
    if (image->framebuffer)
      vkDestroyFramebuffer(ctx->device, image->framebuffer, NULL);
    if (image->view)
      vkDestroyImageView(ctx->device, image->view, NULL);
  }
  if (ctx->swap_images)
    ctx->alloc->free(ctx->swap_images, ctx->alloc->ctx);
  ctx->swap_images = NULL;
  ctx->swap_image_count = 0;
  if (ctx->swapchain)
    vkDestroySwapchainKHR(ctx->device, ctx->swapchain, NULL);
  ctx->swapchain = VK_NULL_HANDLE;
}

static rn_result_t _create_swapchain(struct rn_vk_context *ctx,
                                     uint32_t requested_width,
                                     uint32_t requested_height) {
  if (!requested_width || !requested_height)
    return RN_ERROR_BAD_STATE;

  VkSurfaceCapabilitiesKHR capabilities;
  VkResult status = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      ctx->physical_device, ctx->surface, &capabilities);

  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

  /* the surface capabilities changed to not support color attachment usage,
   * we cannot create a swapchain. */
  if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    return RN_ERROR_BACKEND;

  /* Get surface formats (count) */
  uint32_t format_count = 0;
  status = vkGetPhysicalDeviceSurfaceFormatsKHR(
      ctx->physical_device, ctx->surface, &format_count, NULL);
  if (status != VK_SUCCESS || !format_count)
    return RN_ERROR_BACKEND;

  VkSurfaceFormatKHR *formats =
      ctx->alloc->alloc(format_count * sizeof(*formats), ctx->alloc->ctx);

  if (!formats)
    return RN_ERROR_OUT_OF_MEMORY;

  /* Get surface formats (data) */
  status = vkGetPhysicalDeviceSurfaceFormatsKHR(
      ctx->physical_device, ctx->surface, &format_count, formats);
  if (status != VK_SUCCESS) {
    ctx->alloc->free(formats, ctx->alloc->ctx);
    return _result(ctx, status, "vkGetPhysicalDeviceSurfaceFormatsKHR");
  }

  /* Choose new swapchain format */
  VkSurfaceFormatKHR chosen = formats[0];
  if (ctx->render_pass) {
    bool found = false;
    for (uint32_t i = 0; i < format_count; ++i)
      if (formats[i].format == ctx->swap_format ||
          (format_count == 1 && formats[i].format == VK_FORMAT_UNDEFINED)) {
        chosen = formats[i];
        found = true;
        break;
      }
    if (!found) {
      ctx->alloc->free(formats, ctx->alloc->ctx);
      _VK_ERROR(ctx->log, "surface format changed; existing graphics pipelines "
                          "cannot be reused");
      return RN_ERROR_BACKEND;
    }
  } else {
    if (format_count == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
      /* fallback, for undefined format, we use
       * VK_FORMAT_B8G8R8A8_UNORM/VK_COLOR_SPACE_SRGB_NONLINEAR_KHR */
      chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
      chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    } else {
      /* choose VK_FORMAT_B8G8R8A8_UNORM/VK_COLOR_SPACE_SRGB_NONLINEAR_KHR */
      for (uint32_t i = 0; i < format_count; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
          chosen = formats[i];
          break;
        }
    }
    ctx->swap_format = chosen.format;
  }

  /* done choosing formats */ 
  ctx->alloc->free(formats, ctx->alloc->ctx);

  VkExtent2D extent = capabilities.currentExtent;
  if (extent.width == UINT32_MAX) {
    extent.width = requested_width;
    extent.height = requested_height;
    if (extent.width < capabilities.minImageExtent.width)
      extent.width = capabilities.minImageExtent.width;
    if (extent.width > capabilities.maxImageExtent.width)
      extent.width = capabilities.maxImageExtent.width;
    if (extent.height < capabilities.minImageExtent.height)
      extent.height = capabilities.minImageExtent.height;
    if (extent.height > capabilities.maxImageExtent.height)
      extent.height = capabilities.maxImageExtent.height;
  }
  if (!extent.width || !extent.height)
    return RN_ERROR_BAD_STATE;

  /* create compatible render pass for the swapchain with it's format */
  if (!ctx->render_pass) {
    rn_result_t result = _create_render_pass(ctx, ctx->swap_format);
    if (result != RN_OK)
      return result;
  }

  uint32_t image_count = capabilities.minImageCount + 1;
  if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
    image_count = capabilities.maxImageCount;

  const uint32_t families[2] = {ctx->queue_family, ctx->present_family};
  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

  if (!(capabilities.supportedCompositeAlpha & alpha)) {
    const VkCompositeAlphaFlagBitsKHR choices[] = {
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i)
      if (capabilities.supportedCompositeAlpha & choices[i]) {
        alpha = choices[i];
        break;
      }
  }

  const VkSwapchainCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = ctx->surface,
      .minImageCount = image_count,
      .imageFormat = ctx->swap_format,
      .imageColorSpace = chosen.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = ctx->queue_family == ctx->present_family
                              ? VK_SHARING_MODE_EXCLUSIVE
                              : VK_SHARING_MODE_CONCURRENT,
      .queueFamilyIndexCount =
          ctx->queue_family == ctx->present_family ? 0u : 2u,
      .pQueueFamilyIndices =
          ctx->queue_family == ctx->present_family ? NULL : families,
      .preTransform = capabilities.currentTransform,
      .compositeAlpha = alpha,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
  };

  /* Called only after vkDeviceWaitIdle, which is called before, by the caller */ 
  _destroy_swapchain(ctx);

  /* create new swapchain */
  status = vkCreateSwapchainKHR(ctx->device, &info, NULL, &ctx->swapchain);
  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkCreateSwapchainKHR");

  /* Get new swapchain images, to allocate vkImages, vkImageViews and eventually
   * framebuffers from */
  status =
      vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &image_count, NULL);
  if (status != VK_SUCCESS || !image_count)
    return RN_ERROR_BACKEND;

  /* allocate temp images */
  VkImage *images =
      ctx->alloc->alloc(image_count * sizeof(*images), ctx->alloc->ctx);
  if (!images)
    return RN_ERROR_OUT_OF_MEMORY;

  status = vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &image_count,
                                   images);
  if (status != VK_SUCCESS) {
    ctx->alloc->free(images, ctx->alloc->ctx);
    return _result(ctx, status, "vkGetSwapchainImagesKHR");
  }

  /* allocate retained swapchain images */
  ctx->swap_images = ctx->alloc->alloc(image_count * sizeof(*ctx->swap_images),
                                       ctx->alloc->ctx);
  if (!ctx->swap_images) {
    /* destroy temp images */
    ctx->alloc->free(images, ctx->alloc->ctx);
    return RN_ERROR_OUT_OF_MEMORY;
  }

  memset(ctx->swap_images, 0, image_count * sizeof(*ctx->swap_images));
  ctx->swap_image_count = image_count;

  for (uint32_t i = 0; i < image_count; ++i) {
    struct rn_vk_swap_image *target = &ctx->swap_images[i];
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = images[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = ctx->swap_format,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    /* create image view */
    status = vkCreateImageView(ctx->device, &view_info, NULL, &target->view);
    if (status != VK_SUCCESS)
      break;
    const VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = ctx->render_pass,
        .attachmentCount = 1,
        .pAttachments = &target->view,
        .width = extent.width,
        .height = extent.height,
        .layers = 1,
    };
    
    /* create framebuffer */
    status = vkCreateFramebuffer(ctx->device, &framebuffer_info, NULL,
                                 &target->framebuffer);
    if (status != VK_SUCCESS)
      break;
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    /* create semaphore */
    status = vkCreateSemaphore(ctx->device, &semaphore_info, NULL,
                               &target->render_done);
    if (status != VK_SUCCESS)
      break;
  }
  /* destroy temp images */
  ctx->alloc->free(images, ctx->alloc->ctx);

  if (status != VK_SUCCESS) {
    _destroy_swapchain(ctx);
    return _result(ctx, status, "swapchain image resources");
  }
  ctx->swap_extent = extent;
  ctx->requested_width = requested_width;
  ctx->requested_height = requested_height;
  ctx->resize_pending = false;
  _VK_TRACE(ctx->log, "created swapchain (%ux%u, %u images)", extent.width,
            extent.height, image_count);
  return RN_OK;
}

static rn_result_t _create_frame_commands(struct rn_vk_context *ctx) {
  const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = ctx->queue_family,
  };

  /* create command pool */
  rn_result_t result = _result(
      ctx, vkCreateCommandPool(ctx->device, &pool_info, NULL, &ctx->frame_pool),
      "vkCreateCommandPool(frame)");

  if (result != RN_OK)
    return result;
  const VkCommandBufferAllocateInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx->frame_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };

  /* create frame command buffer */
  result = _result(
      ctx,
      vkAllocateCommandBuffers(ctx->device, &command_info, &ctx->frame_command),
      "vkAllocateCommandBuffers(frame)");
  if (result != RN_OK)
    return result;

  if (ctx->mode == RN_VK_TARGET_MANAGED) {
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

  /* create image available semaphore */
    result = _result(ctx,
                     vkCreateSemaphore(ctx->device, &semaphore_info, NULL,
                                       &ctx->image_available),
                     "vkCreateSemaphore(image available)");

    if (result != RN_OK)
      return result;
  }

  const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
  };

  /* create frame fence */
  return _result(
      ctx, vkCreateFence(ctx->device, &fence_info, NULL, &ctx->frame_fence),
      "vkCreateFence(frame)");
}

static rn_result_t _begin_frame(void *backend_ctx, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height) {
  struct rn_vk_context *ctx = backend_ctx;

  if (!ctx || ctx->frame_active || ctx->fatal || !width || !height)
    return RN_ERROR_BAD_STATE;

  rn_result_t result = _result(
      ctx,
      vkWaitForFences(ctx->device, 1, &ctx->frame_fence, VK_TRUE, UINT64_MAX),
      "vkWaitForFences(frame)");

  if (result != RN_OK)
    return result;

  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkExtent2D extent = {0};
  struct rn_vk_external_target *target = NULL;

  if (ctx->mode == RN_VK_TARGET_MANAGED) {
    uint32_t framebuffer_width = width;
    uint32_t framebuffer_height = height;

    ctx->platform.framebuffer_size(ctx->platform.user, &framebuffer_width,
                                   &framebuffer_height);

    if (!framebuffer_width || !framebuffer_height)
      return RN_ERROR_BAD_STATE;

    /* pending resizes need to wait for idle */
    if (ctx->resize_pending || framebuffer_width != ctx->requested_width ||
        framebuffer_height != ctx->requested_height) {
      result = _result(ctx, vkDeviceWaitIdle(ctx->device),
                       "vkDeviceWaitIdle(resize)");

      if (result != RN_OK)
        return result;

      result = _create_swapchain(ctx, framebuffer_width, framebuffer_height);
      if (result != RN_OK)
        return result;
    }

    VkResult status = VK_SUCCESS;

    /* acquire the next image from the managed swapchain. simple. */
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
      status = vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX,
                                     ctx->image_available, VK_NULL_HANDLE,
                                     &ctx->image_index);

      if (status != VK_ERROR_OUT_OF_DATE_KHR)
        break;

      /* out of date swapchains must be recreated (and we must wait for idle
       * first) */

      result = _result(ctx, vkDeviceWaitIdle(ctx->device),
                       "vkDeviceWaitIdle(acquire)");

      if (result != RN_OK)
        return result;

      result = _create_swapchain(ctx, framebuffer_width, framebuffer_height);
      if (result != RN_OK)
        return result;
    }

    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
      return _result(ctx, status, "vkAcquireNextImageKHR");

    if (status == VK_SUBOPTIMAL_KHR)
      ctx->resize_pending = true;

    /* get the framebuffer of the swapchain image at the current, returned
     * index. */
    framebuffer = ctx->swap_images[ctx->image_index].framebuffer;
    extent = ctx->swap_extent;
  } else {
    target = _external_target(ctx, ctx->active_target);

    if (!target)
      return RN_ERROR_BAD_STATE;

    framebuffer = target->framebuffer;
    extent = target->extent;
  }

  result = _result(ctx, vkResetCommandPool(ctx->device, ctx->frame_pool, 0),
                   "vkResetCommandPool(frame)");

  if (result != RN_OK)
    return result;

  /* command buffer use is began for both managed and external mode */
  const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };

  result = _result(ctx, vkBeginCommandBuffer(ctx->frame_command, &begin),
                   "vkBeginCommandBuffer(frame)");

  if (result != RN_OK)
    return result;

  if (target) {
    /* acquire a borrowed image from the external presentation engine */
    const VkImageMemoryBarrier acquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout =
            target->used ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL_KHR,
        .dstQueueFamilyIndex = ctx->queue_family,
        .image = target->image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };

    vkCmdPipelineBarrier(ctx->frame_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &acquire);
  }

  /* render pass is began for both managed and external mode */
  const VkRenderPassBeginInfo pass = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = ctx->render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, extent},
  };

  vkCmdBeginRenderPass(ctx->frame_command, &pass, VK_SUBPASS_CONTENTS_INLINE);

  ctx->command_buffer = ctx->frame_command;
  ctx->frame_extent = extent;
  ctx->frame_active = true;
  ctx->extent_x = x;
  ctx->extent_y = y;
  ctx->extent_width = width;
  ctx->extent_height = height;
  ctx->pipeline = RN_INVALID_HANDLE;
  ctx->index_buffer = RN_INVALID_HANDLE;

  for (uint32_t i = 0; i < RN_VK_MAX_BINDINGS; ++i)
    ctx->vertex_buffers[i] = RN_INVALID_HANDLE;

  for (uint32_t i = 0; i < RN_VK_MAX_TEXTURES; ++i)
    ctx->bound_textures[i] = RN_INVALID_HANDLE;

  VkRect2D scissor = {{(int32_t)x, (int32_t)y}, {width, height}};

  if (x >= extent.width || y >= extent.height) {
    scissor.extent = (VkExtent2D){0, 0};
  } else {
    if (scissor.extent.width > extent.width - x)
      scissor.extent.width = extent.width - x;

    if (scissor.extent.height > extent.height - y)
      scissor.extent.height = extent.height - y;
  }

  const VkViewport viewport = {
      (float)x, (float)y, (float)width, (float)height, 0.0f, 1.0f,
  };

  vkCmdSetViewport(ctx->frame_command, 0, 1, &viewport);
  vkCmdSetScissor(ctx->frame_command, 0, 1, &scissor);

  return RN_OK;
}

static rn_result_t _clear_color(void *backend_ctx, rn_color_t color) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !ctx->frame_active)
    return RN_ERROR_BAD_STATE;

  const VkClearAttachment attachment = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .colorAttachment = 0,
      .clearValue = {.color = {.float32 = {color.r, color.g, color.b,
                                           color.a}}},
  };

  const VkClearRect region = {
      .rect = {{0, 0}, ctx->frame_extent},
      .baseArrayLayer = 0,
      .layerCount = 1,
  };

  vkCmdClearAttachments(ctx->command_buffer, 1, &attachment, 1, &region);
  return RN_OK;
}

static rn_result_t _end_frame(void *backend_ctx) {
  struct rn_vk_context *ctx = backend_ctx;

  if (!ctx || !ctx->frame_active)
    return RN_ERROR_BAD_STATE;

  vkCmdEndRenderPass(ctx->command_buffer);

  if (ctx->mode == RN_VK_TARGET_EXTERNAL) {
    struct rn_vk_external_target *target =
        _external_target(ctx, ctx->active_target);

    if (!target)
      return RN_ERROR_BAD_STATE;

    /* release the queue ownership so a presentation-engine consumer can read
     * the dmabuf */
    const VkImageMemoryBarrier release = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = ctx->queue_family,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL_KHR,
        .image = target->image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };

    vkCmdPipelineBarrier(
        ctx->frame_command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &release);

    target->used = true;
  }

  /* end the command buffer, in all modes */
  rn_result_t result = _result(ctx, vkEndCommandBuffer(ctx->command_buffer),
                               "vkEndCommandBuffer(frame)");

  ctx->command_buffer = VK_NULL_HANDLE;
  ctx->frame_active = false;

  if (result != RN_OK)
    return result;

  /* reset the fences, in all modes */
  result = _result(ctx, vkResetFences(ctx->device, 1, &ctx->frame_fence),
                   "vkResetFences(frame)");

  if (result != RN_OK)
    return result;

  const VkPipelineStageFlags wait_stage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphore render_done = VK_NULL_HANDLE;

  if (ctx->mode == RN_VK_TARGET_MANAGED)
    render_done = ctx->swap_images[ctx->image_index].render_done;

  const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = ctx->mode == RN_VK_TARGET_MANAGED ? 1u : 0u,
      .pWaitSemaphores =
          ctx->mode == RN_VK_TARGET_MANAGED ? &ctx->image_available : NULL,
      .pWaitDstStageMask =
          ctx->mode == RN_VK_TARGET_MANAGED ? &wait_stage : NULL,
      .commandBufferCount = 1,
      .pCommandBuffers = &ctx->frame_command,
      .signalSemaphoreCount = ctx->mode == RN_VK_TARGET_MANAGED ? 1u : 0u,
      .pSignalSemaphores =
          ctx->mode == RN_VK_TARGET_MANAGED ? &render_done : NULL,
  };

  VkResult status = vkQueueSubmit(ctx->queue, 1, &submit, ctx->frame_fence);

  if (status != VK_SUCCESS) {
    ctx->fatal = true;
    return _result(ctx, status, "vkQueueSubmit(frame)");
  }

  ctx->frame_submitted = true;

  if (ctx->mode == RN_VK_TARGET_EXTERNAL)
    return RN_OK;

  const VkPresentInfoKHR present = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &render_done,
      .swapchainCount = 1,
      .pSwapchains = &ctx->swapchain,
      .pImageIndices = &ctx->image_index,
  };

  status = vkQueuePresentKHR(ctx->present_queue, &present);
  if (status == VK_SUBOPTIMAL_KHR || status == VK_ERROR_OUT_OF_DATE_KHR) {
    ctx->resize_pending = true;
    return RN_OK;
  }

  return _result(ctx, status, "vkQueuePresentKHR");
}

static rn_result_t _create_buffer(void *backend_ctx,
                                  const struct rn_buffer_desc_t *desc,
                                  rn_buffer_handle_t *out) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !desc || !out || !desc->size || desc->size > SIZE_MAX - 1 ||
      (desc->target != RN_BUFFER_VERTEX && desc->target != RN_BUFFER_INDEX))
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_vk_buffer *entries =
      _grow(ctx, ctx->buffers, ctx->buffer_count, sizeof(*entries));
  if (!entries)
    return RN_ERROR_OUT_OF_MEMORY;
  ctx->buffers = entries;

  const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = (VkDeviceSize)desc->size,
      .usage = (desc->target == RN_BUFFER_INDEX
                    ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                    : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  const VmaAllocationCreateInfo alloc_info = {.usage =
                                                  VMA_MEMORY_USAGE_CPU_TO_GPU};
  struct rn_vk_buffer buffer = {.size = (VkDeviceSize)desc->size,
                                .target = desc->target};

  VkResult status = vmaCreateBuffer(ctx->vma, &buffer_info, &alloc_info,
                                    &buffer.buffer, &buffer.allocation, NULL);
  if (status != VK_SUCCESS)
    return _result(ctx, status, "vmaCreateBuffer");
  buffer.alive = true;

  const rn_buffer_handle_t handle = (rn_buffer_handle_t)ctx->buffer_count;
  ctx->buffers[ctx->buffer_count++] = buffer;
  if (desc->data) {
    rn_result_t result =
        _write_buffer(ctx, handle, 0, 0, desc->data, desc->size);
    if (result != RN_OK) {
      _destroy_buffer(ctx, handle);
      return result;
    }
  }

  *out = handle;
  _VK_TRACE(ctx->log, "created buffer %u (%zu bytes)", (unsigned)handle,
            desc->size);
  return RN_OK;
}

static void _destroy_buffer(void *backend_ctx, rn_buffer_handle_t handle) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx)
    return;
  struct rn_vk_buffer *buffer = _buffer(ctx, handle);
  if (!buffer)
    return;
  if (ctx->frame_active) {
    _VK_ERROR(ctx->log, "cannot destroy a buffer during a frame");
    return;
  }
  if (!ctx->tearing_down)
    vkDeviceWaitIdle(ctx->device);
  vmaDestroyBuffer(ctx->vma, buffer->buffer, buffer->allocation);
  buffer->alive = false;
  _VK_TRACE(ctx->log, "destroyed buffer %u", (unsigned)handle);
}

static rn_result_t _write_buffer(void *backend_ctx, rn_buffer_handle_t handle,
                                 uint32_t binding, size_t offset,
                                 const void *data, size_t size) {
  (void)binding;
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || (!data && size))
    return RN_ERROR_INVALID_ARGUMENT;
  struct rn_vk_buffer *buffer = _buffer(ctx, handle);
  if (!buffer || (VkDeviceSize)offset > buffer->size ||
      (VkDeviceSize)size > buffer->size - offset)
    return RN_ERROR_INVALID_ARGUMENT;
  if (!size)
    return RN_OK;

  void *mapped = NULL;
  VkResult status = vmaMapMemory(ctx->vma, buffer->allocation, &mapped);
  if (status != VK_SUCCESS)
    return _result(ctx, status, "vmaMapMemory(buffer)");
  memcpy((unsigned char *)mapped + offset, data, size);
  status = vmaFlushAllocation(ctx->vma, buffer->allocation, offset, size);
  vmaUnmapMemory(ctx->vma, buffer->allocation);
  return _result(ctx, status, "vmaFlushAllocation(buffer)");
}

static rn_result_t _set_vertex_buffer(void *backend_ctx,
                                      rn_buffer_handle_t handle,
                                      uint32_t binding) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !ctx->frame_active || binding >= RN_VK_MAX_BINDINGS)
    return RN_ERROR_BAD_STATE;
  struct rn_vk_buffer *buffer = _buffer(ctx, handle);
  if (!buffer || buffer->target != RN_BUFFER_VERTEX)
    return RN_ERROR_INVALID_ARGUMENT;

  const VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(ctx->command_buffer, binding, 1, &buffer->buffer,
                         &offset);
  ctx->vertex_buffers[binding] = handle;
  return RN_OK;
}

static rn_result_t _set_index_buffer(void *backend_ctx,
                                     rn_buffer_handle_t handle) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !ctx->frame_active)
    return RN_ERROR_BAD_STATE;
  struct rn_vk_buffer *buffer = _buffer(ctx, handle);
  if (!buffer || buffer->target != RN_BUFFER_INDEX)
    return RN_ERROR_INVALID_ARGUMENT;
  vkCmdBindIndexBuffer(ctx->command_buffer, buffer->buffer, 0,
                       VK_INDEX_TYPE_UINT32);
  ctx->index_buffer = handle;
  return RN_OK;
}

static rn_result_t _shader_module(struct rn_vk_context *ctx,
                                  const uint32_t *code, size_t size,
                                  VkShaderModule *module) {
  const VkShaderModuleCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size,
      .pCode = code,
  };
  return _result(ctx, vkCreateShaderModule(ctx->device, &info, NULL, module),
                 "vkCreateShaderModule");
}

static rn_result_t _create_pipeline(void *backend_ctx,
                                    const struct rn_pipeline_desc_t *desc,
                                    rn_pipeline_handle_t *out) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !desc || !out || !ctx->render_pass || desc->depth_test ||
      desc->vertex_binding_count > RN_VK_MAX_BINDINGS ||
      (desc->vertex_binding_count && !desc->vertex_bindings) ||
      (desc->uniform_binding_count && !desc->uniform_bindings) ||
      desc->shader > RN_PIPELINE_UI_QUAD_DEDICATED ||
      desc->blend > RN_BLEND_PREMULTIPLIED_ALPHA)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_vk_pipeline *entries =
      _grow(ctx, ctx->pipelines, ctx->pipeline_count, sizeof(*entries));
  if (!entries)
    return RN_ERROR_OUT_OF_MEMORY;
  ctx->pipelines = entries;
  struct rn_vk_pipeline pipeline = {.shader = desc->shader};

  VkVertexInputBindingDescription bindings[RN_VK_MAX_BINDINGS] = {0};
  VkVertexInputAttributeDescription attributes[32] = {0};
  uint32_t attribute_count = 0;
  for (size_t binding_index = 0; binding_index < desc->vertex_binding_count;
       ++binding_index) {
    const struct rn_vertex_buffer_layout_t *layout =
        &desc->vertex_bindings[binding_index];
    if (layout->binding >= RN_VK_MAX_BINDINGS || !layout->stride ||
        (layout->attribute_count && !layout->attributes) ||
        layout->attribute_count > 32 - attribute_count)
      return RN_ERROR_INVALID_ARGUMENT;

    pipeline.strides[layout->binding] = layout->stride;
    bindings[binding_index] = (VkVertexInputBindingDescription){
        .binding = layout->binding,
        .stride = layout->stride,
        .inputRate = layout->step_mode == RN_STEP_INSTANCE
                         ? VK_VERTEX_INPUT_RATE_INSTANCE
                         : VK_VERTEX_INPUT_RATE_VERTEX,
    };
    for (size_t attribute_index = 0; attribute_index < layout->attribute_count;
         ++attribute_index) {
      const struct rn_vertex_attribute_t *attribute =
          &layout->attributes[attribute_index];
      uint32_t attribute_size =
          attribute->format == RN_VERTEX_FLOAT32X2 ? 8u : 16u;
      if (attribute->format > RN_VERTEX_FLOAT32X4 ||
          attribute->offset > layout->stride ||
          attribute_size > layout->stride - attribute->offset)
        return RN_ERROR_INVALID_ARGUMENT;
      attributes[attribute_count++] = (VkVertexInputAttributeDescription){
          .location = attribute->location,
          .binding = layout->binding,
          .format = attribute->format == RN_VERTEX_FLOAT32X2
                        ? VK_FORMAT_R32G32_SFLOAT
                        : VK_FORMAT_R32G32B32A32_SFLOAT,
          .offset = attribute->offset,
      };
    }
  }

  /* Atlas uses one combined sampler. Dedicated batches use sixteen slots. */
  pipeline.descriptor_count =
      desc->shader == RN_PIPELINE_UI_QUAD_ATLAS ? 1u : RN_VK_MAX_TEXTURES;
  const VkDescriptorSetLayoutBinding sampler_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = pipeline.descriptor_count,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  const VkDescriptorSetLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &sampler_binding,
  };
  rn_result_t result =
      _result(ctx,
              vkCreateDescriptorSetLayout(ctx->device, &layout_info, NULL,
                                          &pipeline.set_layout),
              "vkCreateDescriptorSetLayout");
  if (result != RN_OK)
    return result;

  const VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
      .offset = 0,
      .size = 8,
  };
  const VkPipelineLayoutCreateInfo pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &pipeline.set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
  };
  result = _result(ctx,
                   vkCreatePipelineLayout(ctx->device, &pipeline_layout_info,
                                          NULL, &pipeline.layout),
                   "vkCreatePipelineLayout");
  if (result != RN_OK)
    goto fail;

  VkShaderModule vertex_module = VK_NULL_HANDLE;
  VkShaderModule fragment_module = VK_NULL_HANDLE;
  result = _shader_module(ctx, rn_vk_vertex_spv, rn_vk_vertex_spv_size,
                          &vertex_module);
  if (result != RN_OK)
    goto fail;

  const uint32_t *fragment_code = desc->shader == RN_PIPELINE_UI_QUAD_ATLAS
                                      ? rn_vk_atlas_spv
                                      : rn_vk_dedicated_spv;
  size_t fragment_size = desc->shader == RN_PIPELINE_UI_QUAD_ATLAS
                             ? rn_vk_atlas_spv_size
                             : rn_vk_dedicated_spv_size;
  result = _shader_module(ctx, fragment_code, fragment_size, &fragment_module);
  if (result != RN_OK) {
    vkDestroyShaderModule(ctx->device, vertex_module, NULL);
    goto fail;
  }

  const VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vertex_module,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fragment_module,
       .pName = "main"},
  };
  const VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = (uint32_t)desc->vertex_binding_count,
      .pVertexBindingDescriptions = bindings,
      .vertexAttributeDescriptionCount = attribute_count,
      .pVertexAttributeDescriptions = attributes,
  };
  const VkPipelineInputAssemblyStateCreateInfo assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineViewportStateCreateInfo viewport = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
  };
  const VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const VkPipelineColorBlendAttachmentState attachment = {
      .blendEnable = desc->blend == RN_BLEND_OPAQUE ? VK_FALSE : VK_TRUE,
      .srcColorBlendFactor = desc->blend == RN_BLEND_PREMULTIPLIED_ALPHA
                                 ? VK_BLEND_FACTOR_ONE
                                 : VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  const VkPipelineColorBlendStateCreateInfo blending = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
  };
  const VkPipelineDepthStencilStateCreateInfo depth = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = desc->depth_test ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = VK_FALSE,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
  };
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                           VK_DYNAMIC_STATE_SCISSOR};
  const VkPipelineDynamicStateCreateInfo dynamic = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dynamic_states,
  };
  const VkGraphicsPipelineCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &assembly,
      .pViewportState = &viewport,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pDepthStencilState = &depth,
      .pColorBlendState = &blending,
      .pDynamicState = &dynamic,
      .layout = pipeline.layout,
      .renderPass = ctx->render_pass,
      .subpass = 0,
  };
  VkResult status = vkCreateGraphicsPipelines(
      ctx->device, VK_NULL_HANDLE, 1, &create_info, NULL, &pipeline.pipeline);
  vkDestroyShaderModule(ctx->device, fragment_module, NULL);
  vkDestroyShaderModule(ctx->device, vertex_module, NULL);
  result = _result(ctx, status, "vkCreateGraphicsPipelines");
  if (result != RN_OK)
    goto fail;

  pipeline.alive = true;
  *out = (rn_pipeline_handle_t)ctx->pipeline_count;
  ctx->pipelines[ctx->pipeline_count++] = pipeline;
  _VK_TRACE(ctx->log, "created %s pipeline %u (%u attributes)",
            desc->shader == RN_PIPELINE_UI_QUAD_ATLAS ? "atlas" : "dedicated",
            (unsigned)*out, attribute_count);
  return RN_OK;

fail:
  if (pipeline.pipeline)
    vkDestroyPipeline(ctx->device, pipeline.pipeline, NULL);
  if (pipeline.layout)
    vkDestroyPipelineLayout(ctx->device, pipeline.layout, NULL);
  if (pipeline.set_layout)
    vkDestroyDescriptorSetLayout(ctx->device, pipeline.set_layout, NULL);
  return result;
}

static void _destroy_pipeline(void *backend_ctx, rn_pipeline_handle_t handle) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx)
    return;
  struct rn_vk_pipeline *pipeline = _pipeline(ctx, handle);
  if (!pipeline)
    return;

  if (ctx->frame_active) {
    _VK_ERROR(ctx->log, "cannot destroy a pipeline during a frame");
    return;
  }
  if (!ctx->tearing_down)
    vkDeviceWaitIdle(ctx->device);
  struct rn_vk_descriptor_entry *entry = pipeline->descriptors;
  while (entry) {
    struct rn_vk_descriptor_entry *next = entry->next;
    ctx->alloc->free(entry, ctx->alloc->ctx);
    entry = next;
  }
  struct rn_vk_descriptor_page *page = pipeline->pages;
  while (page) {
    struct rn_vk_descriptor_page *next = page->next;
    vkDestroyDescriptorPool(ctx->device, page->pool, NULL);
    ctx->alloc->free(page, ctx->alloc->ctx);
    page = next;
  }
  vkDestroyPipeline(ctx->device, pipeline->pipeline, NULL);
  vkDestroyPipelineLayout(ctx->device, pipeline->layout, NULL);
  vkDestroyDescriptorSetLayout(ctx->device, pipeline->set_layout, NULL);
  pipeline->alive = false;
  if (ctx->pipeline == handle)
    ctx->pipeline = RN_INVALID_HANDLE;
  _VK_TRACE(ctx->log, "destroyed pipeline %u", (unsigned)handle);
}

static rn_result_t _set_pipeline(void *backend_ctx,
                                 rn_pipeline_handle_t handle) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !ctx->frame_active)
    return RN_ERROR_BAD_STATE;
  struct rn_vk_pipeline *pipeline = _pipeline(ctx, handle);
  if (!pipeline)
    return RN_ERROR_INVALID_ARGUMENT;
  vkCmdBindPipeline(ctx->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline->pipeline);
  ctx->pipeline = handle;
  return RN_OK;
}

static rn_result_t _set_uniform_2f(void *backend_ctx,
                                   rn_pipeline_handle_t handle,
                                   const char *name, float x, float y) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !ctx->frame_active || !name || ctx->pipeline != handle ||
      strcmp(name, "u_screen_size"))
    return RN_ERROR_BAD_STATE;
  struct rn_vk_pipeline *pipeline = _pipeline(ctx, handle);
  if (!pipeline)
    return RN_ERROR_INVALID_ARGUMENT;
  const float values[2] = {x, y};
  vkCmdPushConstants(ctx->command_buffer, pipeline->layout,
                     VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(values), values);
  return RN_OK;
}

static rn_result_t _set_texture(void *backend_ctx, uint32_t slot,
                                rn_texture_handle_t handle) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !ctx->frame_active || slot >= RN_VK_MAX_TEXTURES)
    return RN_ERROR_BAD_STATE;
  struct rn_vk_texture *texture = _texture(ctx, handle);
  struct rn_vk_pipeline *pipeline = _pipeline(ctx, ctx->pipeline);
  if (!texture || !pipeline ||
      (pipeline->shader == RN_PIPELINE_UI_QUAD_ATLAS &&
       (!texture->array || slot != 0)) ||
      (pipeline->shader == RN_PIPELINE_UI_QUAD_DEDICATED && texture->array))
    return RN_ERROR_INVALID_ARGUMENT;
  ctx->bound_textures[slot] = handle;
  return RN_OK;
}

/* Vulkan descriptor sets must be immutable once recorded into an in-flight
 * command buffer. Cache a set for each unique texture combination, never
 * rewrite a previously used set. The pool grows by pages when needed. */
static rn_result_t _descriptor_for_draw(struct rn_vk_context *ctx,
                                        struct rn_vk_pipeline *pipeline,
                                        VkDescriptorSet *out) {
  const uint32_t count = pipeline->descriptor_count;
  rn_texture_handle_t textures[RN_VK_MAX_TEXTURES];
  rn_texture_handle_t fallback = ctx->bound_textures[0];
  if (!_texture(ctx, fallback))
    return RN_ERROR_BAD_STATE;
  for (uint32_t slot = 0; slot < count; ++slot) {
    rn_texture_handle_t handle = ctx->bound_textures[slot];
    textures[slot] = _texture(ctx, handle) ? handle : fallback;
  }

  for (struct rn_vk_descriptor_entry *entry = pipeline->descriptors; entry;
       entry = entry->next) {
    if (entry->count == count &&
        !memcmp(textures, entry->textures, count * sizeof(*textures))) {
      *out = entry->set;
      return RN_OK;
    }
  }

  struct rn_vk_descriptor_entry *entry =
      ctx->alloc->alloc(sizeof(*entry), ctx->alloc->ctx);
  if (!entry)
    return RN_ERROR_OUT_OF_MEMORY;
  memset(entry, 0, sizeof(*entry));
  entry->count = count;
  memcpy(entry->textures, textures, count * sizeof(*textures));

  struct rn_vk_descriptor_page *page = pipeline->pages;
  if (!page || page->used == RN_VK_DESCRIPTOR_PAGE_SIZE) {
    struct rn_vk_descriptor_page *new_page =
        ctx->alloc->alloc(sizeof(*new_page), ctx->alloc->ctx);
    if (!new_page) {
      ctx->alloc->free(entry, ctx->alloc->ctx);
      return RN_ERROR_OUT_OF_MEMORY;
    }
    memset(new_page, 0, sizeof(*new_page));
    const VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = RN_VK_DESCRIPTOR_PAGE_SIZE * count,
    };
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = RN_VK_DESCRIPTOR_PAGE_SIZE,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VkResult status =
        vkCreateDescriptorPool(ctx->device, &pool_info, NULL, &new_page->pool);
    if (status != VK_SUCCESS) {
      ctx->alloc->free(new_page, ctx->alloc->ctx);
      ctx->alloc->free(entry, ctx->alloc->ctx);
      return _result(ctx, status, "vkCreateDescriptorPool");
    }
    new_page->next = pipeline->pages;
    pipeline->pages = new_page;
    page = new_page;
    _VK_TRACE(ctx->log,
              "created descriptor page (%u sets, %u textures per set)",
              RN_VK_DESCRIPTOR_PAGE_SIZE, count);
  }

  const VkDescriptorSetAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = page->pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &pipeline->set_layout,
  };
  VkResult status =
      vkAllocateDescriptorSets(ctx->device, &alloc_info, &entry->set);
  if (status != VK_SUCCESS) {
    ctx->alloc->free(entry, ctx->alloc->ctx);
    return _result(ctx, status, "vkAllocateDescriptorSets");
  }
  ++page->used;

  VkDescriptorImageInfo images[RN_VK_MAX_TEXTURES] = {0};
  for (uint32_t slot = 0; slot < count; ++slot) {
    struct rn_vk_texture *texture = _texture(ctx, textures[slot]);
    images[slot] = (VkDescriptorImageInfo){
        .sampler = texture->sampler,
        .imageView = texture->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
  }
  const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = entry->set,
      .dstBinding = 0,
      .descriptorCount = count,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = images,
  };
  vkUpdateDescriptorSets(ctx->device, 1, &write, 0, NULL);
  entry->next = pipeline->descriptors;
  pipeline->descriptors = entry;
  *out = entry->set;
  return RN_OK;
}

static rn_result_t
_draw_indexed_instanced(void *backend_ctx,
                        const struct rn_draw_indexed_instanced_t *draw) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !ctx->frame_active || !draw)
    return RN_ERROR_BAD_STATE;
  struct rn_vk_pipeline *pipeline = _pipeline(ctx, ctx->pipeline);
  struct rn_vk_buffer *index = _buffer(ctx, ctx->index_buffer);
  if (!pipeline || !index || draw->index_offset > index->size / 4u ||
      draw->index_count > index->size / 4u - draw->index_offset)
    return RN_ERROR_BAD_STATE;

  /* The quad instance buffer is bound at binding 1, and Vulkan 1.0 supports
   * firstInstance in vkCmdDrawIndexed without an extension. */
  VkDescriptorSet set = VK_NULL_HANDLE;
  rn_result_t result = _descriptor_for_draw(ctx, pipeline, &set);
  if (result != RN_OK)
    return result;
  vkCmdBindDescriptorSets(ctx->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline->layout, 0, 1, &set, 0, NULL);
  vkCmdDrawIndexed(ctx->command_buffer, draw->index_count, draw->instance_count,
                   draw->index_offset, draw->vertex_offset,
                   draw->instance_offset);
  return RN_OK;
}

/* Uploads are synchronous, use a private command pool and Runara-owned
 * graphics queue, and happen outside of the active rendering pass. */
static rn_result_t _begin_upload(struct rn_vk_context *ctx) {
  VkResult status =
      vkWaitForFences(ctx->device, 1, &ctx->upload_fence, VK_TRUE, UINT64_MAX);
  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkWaitForFences(upload)");
  status = vkResetCommandPool(ctx->device, ctx->upload_pool, 0);
  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkResetCommandPool(upload)");
  const VkCommandBufferBeginInfo info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  return _result(ctx, vkBeginCommandBuffer(ctx->upload_command, &info),
                 "vkBeginCommandBuffer(upload)");
}

static rn_result_t _end_upload(struct rn_vk_context *ctx) {
  VkResult status = vkEndCommandBuffer(ctx->upload_command);
  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkEndCommandBuffer(upload)");
  status = vkResetFences(ctx->device, 1, &ctx->upload_fence);
  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkResetFences(upload)");
  const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &ctx->upload_command,
  };
  status = vkQueueSubmit(ctx->queue, 1, &submit, ctx->upload_fence);
  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkQueueSubmit(upload)");
  return _result(
      ctx,
      vkWaitForFences(ctx->device, 1, &ctx->upload_fence, VK_TRUE, UINT64_MAX),
      "vkWaitForFences(upload complete)");
}

static void _image_barrier(struct rn_vk_context *ctx, VkImage image,
                           VkImageLayout old_layout, VkImageLayout new_layout,
                           uint32_t layers) {
  const VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                           ? VK_ACCESS_TRANSFER_WRITE_BIT
                           : 0,
      .dstAccessMask = new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                           ? VK_ACCESS_TRANSFER_WRITE_BIT
                           : VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = layers,
          },
  };
  const VkPipelineStageFlags src_stage =
      old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
          ? VK_PIPELINE_STAGE_TRANSFER_BIT
      : old_layout == VK_IMAGE_LAYOUT_UNDEFINED
          ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
          : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  const VkPipelineStageFlags dst_stage =
      new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
          ? VK_PIPELINE_STAGE_TRANSFER_BIT
          : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  vkCmdPipelineBarrier(ctx->upload_command, src_stage, dst_stage, 0, 0, NULL, 0,
                       NULL, 1, &barrier);
}

static rn_result_t _upload_image(struct rn_vk_context *ctx,
                                 struct rn_vk_texture *texture, uint32_t layer,
                                 uint32_t x, uint32_t y, uint32_t width,
                                 uint32_t height, const void *pixels,
                                 size_t size) {
  if (!ctx || !texture || !pixels || ctx->frame_active || !width || !height ||
      x > texture->width || y > texture->height || width > texture->width - x ||
      height > texture->height - y || layer >= texture->layers)
    return RN_ERROR_INVALID_ARGUMENT;
  if ((uint64_t)width * height > SIZE_MAX / 4u ||
      size < (size_t)width * height * 4u)
    return RN_ERROR_INVALID_ARGUMENT;

  const size_t transfer_size = (size_t)width * height * 4u;
  const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = transfer_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  const VmaAllocationCreateInfo allocation_info = {
      .usage = VMA_MEMORY_USAGE_CPU_TO_GPU};
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation allocation = NULL;
  VkResult status = vmaCreateBuffer(ctx->vma, &buffer_info, &allocation_info,
                                    &staging, &allocation, NULL);
  if (status != VK_SUCCESS)
    return _result(ctx, status, "vmaCreateBuffer(texture staging)");

  void *mapped = NULL;
  status = vmaMapMemory(ctx->vma, allocation, &mapped);
  if (status == VK_SUCCESS) {
    memcpy(mapped, pixels, transfer_size);
    status = vmaFlushAllocation(ctx->vma, allocation, 0, transfer_size);
    vmaUnmapMemory(ctx->vma, allocation);
  }
  if (status != VK_SUCCESS) {
    vmaDestroyBuffer(ctx->vma, staging, allocation);
    return _result(ctx, status, "map/flush(texture staging)");
  }

  rn_result_t result = _begin_upload(ctx);
  if (result == RN_OK) {
    _image_barrier(ctx, texture->image,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture->layers);
    const VkBufferImageCopy copy = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = layer,
                .layerCount = 1,
            },
        .imageOffset = {(int32_t)x, (int32_t)y, 0},
        .imageExtent = {width, height, 1},
    };
    vkCmdCopyBufferToImage(ctx->upload_command, staging, texture->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    _image_barrier(ctx, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture->layers);
    result = _end_upload(ctx);
  }
  vmaDestroyBuffer(ctx->vma, staging, allocation);
  return result;
}

static rn_result_t _create_texture_internal(struct rn_vk_context *ctx,
                                            uint32_t width, uint32_t height,
                                            uint32_t layers, bool array,
                                            const void *pixels, size_t size,
                                            rn_texture_handle_t *out) {
  if (!ctx || !out || !width || !height || !layers || ctx->frame_active)
    return RN_ERROR_INVALID_ARGUMENT;
  if (pixels && ((uint64_t)width * height > SIZE_MAX / 4u ||
                 size < (size_t)width * height * 4u))
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_vk_texture *entries =
      _grow(ctx, ctx->textures, ctx->texture_count, sizeof(*entries));
  if (!entries)
    return RN_ERROR_OUT_OF_MEMORY;
  ctx->textures = entries;

  struct rn_vk_texture texture = {
      .width = width,
      .height = height,
      .layers = layers,
      .array = array,
  };
  const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {width, height, 1},
      .mipLevels = 1,
      .arrayLayers = layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  const VmaAllocationCreateInfo allocation_info = {
      .usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VkResult status = vmaCreateImage(ctx->vma, &image_info, &allocation_info,
                                   &texture.image, &texture.allocation, NULL);
  rn_result_t result = _result(ctx, status, "vmaCreateImage");
  if (result != RN_OK)
    return result;

  const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = texture.image,
      .viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .levelCount = 1,
              .layerCount = layers,
          },
  };
  result = _result(
      ctx, vkCreateImageView(ctx->device, &view_info, NULL, &texture.view),
      "vkCreateImageView");
  if (result != RN_OK)
    goto fail;

  const VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
      .maxAnisotropy = 1.0f,
  };
  result = _result(
      ctx, vkCreateSampler(ctx->device, &sampler_info, NULL, &texture.sampler),
      "vkCreateSampler");
  if (result != RN_OK)
    goto fail;

  result = _begin_upload(ctx);
  if (result != RN_OK)
    goto fail;
  _image_barrier(ctx, texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layers);
  result = _end_upload(ctx);
  if (result != RN_OK)
    goto fail;

  if (pixels) {
    result = _upload_image(ctx, &texture, 0, 0, 0, width, height, pixels, size);
    if (result != RN_OK)
      goto fail;
  }

  texture.alive = true;
  *out = (rn_texture_handle_t)ctx->texture_count;
  ctx->textures[ctx->texture_count++] = texture;
  _VK_TRACE(ctx->log, "created %s texture %u (%ux%u, %u layers)",
            array ? "array" : "dedicated", (unsigned)*out, width, height,
            layers);
  return RN_OK;

fail:
  if (texture.sampler)
    vkDestroySampler(ctx->device, texture.sampler, NULL);
  if (texture.view)
    vkDestroyImageView(ctx->device, texture.view, NULL);
  if (texture.image)
    vmaDestroyImage(ctx->vma, texture.image, texture.allocation);
  return result;
}

static rn_result_t _create_texture(void *backend_ctx,
                                   const struct rn_texture_desc_t *desc,
                                   const void *pixels, size_t size,
                                   rn_texture_handle_t *out) {
  if (!desc || desc->format != RN_TEXTURE_RGBA8 || !pixels)
    return RN_ERROR_INVALID_ARGUMENT;
  return _create_texture_internal(backend_ctx, desc->width, desc->height, 1,
                                  false, pixels, size, out);
}

static rn_result_t
_create_texture_array(void *backend_ctx,
                      const struct rn_texture_array_desc_t *desc,
                      rn_texture_handle_t *out) {
  if (!desc || desc->format != RN_TEXTURE_RGBA8)
    return RN_ERROR_INVALID_ARGUMENT;
  return _create_texture_internal(backend_ctx, desc->width, desc->height,
                                  desc->layers, true, NULL, 0, out);
}

static rn_result_t
_write_texture_array_layer(void *backend_ctx,
                           const struct rn_texture_array_write_t *write,
                           const void *pixels, size_t size) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !write)
    return RN_ERROR_INVALID_ARGUMENT;
  struct rn_vk_texture *texture = _texture(ctx, write->texture);
  if (!texture || !texture->array)
    return RN_ERROR_INVALID_ARGUMENT;
  return _upload_image(ctx, texture, write->layer, write->x, write->y,
                       write->width, write->height, pixels, size);
}

static void _destroy_texture(void *backend_ctx, rn_texture_handle_t handle) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx)
    return;
  struct rn_vk_texture *texture = _texture(ctx, handle);
  if (!texture)
    return;
  if (ctx->frame_active) {
    _VK_ERROR(ctx->log, "cannot destroy a texture during a frame");
    return;
  }
  if (!ctx->tearing_down)
    vkDeviceWaitIdle(ctx->device);
  vkDestroySampler(ctx->device, texture->sampler, NULL);
  vkDestroyImageView(ctx->device, texture->view, NULL);
  if (!texture->borrowed)
    vmaDestroyImage(ctx->vma, texture->image, texture->allocation);
  texture->alive = false;
  _VK_TRACE(ctx->log, "destroyed texture %u", (unsigned)handle);
}

bool rn_vk_backend_implement(struct rn_backend_impl_t *impl,
                             const struct rn_allocator_t *alloc,
                             const struct rn_log_t *log, const void *backend_info) {
  const struct rn_vk_init_options_t *options = backend_info;

  if (!impl || !alloc || !alloc->alloc || !alloc->realloc || !alloc->free ||
      !options ||
      (options->mode != RN_VK_TARGET_MANAGED &&
       options->mode != RN_VK_TARGET_EXTERNAL))
    return false;

  if (options->mode == RN_VK_TARGET_MANAGED &&
      (!options->platform.instance_extensions ||
       !options->platform.create_surface ||
       !options->platform.framebuffer_size))
    return false;

  struct rn_vk_context *ctx = alloc->alloc(sizeof(*ctx), alloc->ctx);
  if (!ctx)
    return false;

  memset(ctx, 0, sizeof(*ctx));
  ctx->alloc = alloc;
  ctx->log = log;
  ctx->config = *options;
  ctx->mode = options->mode;
  ctx->platform = options->platform;
  ctx->active_target = RN_VK_INVALID_TARGET;
  ctx->pipeline = RN_INVALID_HANDLE;
  ctx->index_buffer = RN_INVALID_HANDLE;

  if (_create_instance(ctx) != RN_OK)
    goto failed;

  if (ctx->mode == RN_VK_TARGET_MANAGED) {
    if (_result(ctx,
                ctx->platform.create_surface(ctx->platform.user, ctx->instance,
                                             &ctx->surface),
                "create_surface") != RN_OK)
      goto failed;

    if (!ctx->surface) {
      _VK_ERROR(log, "platform create_surface returned a null VkSurfaceKHR");
      goto failed;
    }
  }

  if (_create_device(ctx) != RN_OK)
    goto failed;

  if (ctx->mode == RN_VK_TARGET_MANAGED) {
    uint32_t width = 0;
    uint32_t height = 0;
    ctx->platform.framebuffer_size(ctx->platform.user, &width, &height);

    if (!width || !height) {
      _VK_ERROR(log, "cannot initialize a zero-sized Vulkan swapchain");
      goto failed;
    }

    if (_create_swapchain(ctx, width, height) != RN_OK)
      goto failed;
  } else {
    VkFormat format = options->external_format;
    if (format == VK_FORMAT_UNDEFINED)
      format = VK_FORMAT_B8G8R8A8_UNORM;

    /* create render pass with the passed external format (or fallback to
     * VK_FORMAT_B8G8R8A8_UNORM)*/
    ctx->swap_format = format;
    if (_create_render_pass(ctx, format) != RN_OK)
      goto failed;
  }

  if (_create_frame_commands(ctx) != RN_OK)
    goto failed;

  const VmaAllocatorCreateInfo vma_info = {
      .physicalDevice = ctx->physical_device,
      .device = ctx->device,
      .instance = ctx->instance,
      .vulkanApiVersion = VK_API_VERSION_1_0,
  };

  if (_result(ctx, vmaCreateAllocator(&vma_info, &ctx->vma),
              "vmaCreateAllocator") != RN_OK)
    goto failed;

  const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = ctx->queue_family,
  };

  if (_result(
          ctx,
          vkCreateCommandPool(ctx->device, &pool_info, NULL, &ctx->upload_pool),
          "vkCreateCommandPool(upload)") != RN_OK)
    goto failed;

  const VkCommandBufferAllocateInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx->upload_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };

  if (_result(ctx,
              vkAllocateCommandBuffers(ctx->device, &command_info,
                                       &ctx->upload_command),
              "vkAllocateCommandBuffers(upload)") != RN_OK)
    goto failed;

  const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
  };

  if (_result(ctx,
              vkCreateFence(ctx->device, &fence_info, NULL, &ctx->upload_fence),
              "vkCreateFence(upload)") != RN_OK)
    goto failed;

  *impl = _rn_vk_dispatch;
  impl->ctx = ctx;

  _VK_TRACE(log, "Vulkan 1.0 backend ready (GPU=%s, mode=%s)",
            ctx->properties.deviceName,
            ctx->mode == RN_VK_TARGET_MANAGED ? "managed" : "external");

  return true;

failed:
  if (ctx->device)
    vkDeviceWaitIdle(ctx->device);

  if (ctx->device && ctx->upload_fence)
    vkDestroyFence(ctx->device, ctx->upload_fence, NULL);

  if (ctx->device && ctx->upload_pool)
    vkDestroyCommandPool(ctx->device, ctx->upload_pool, NULL);

  if (ctx->vma)
    vmaDestroyAllocator(ctx->vma);

  if (ctx->device && ctx->frame_fence)
    vkDestroyFence(ctx->device, ctx->frame_fence, NULL);

  if (ctx->device && ctx->image_available)
    vkDestroySemaphore(ctx->device, ctx->image_available, NULL);

  if (ctx->device && ctx->frame_pool)
    vkDestroyCommandPool(ctx->device, ctx->frame_pool, NULL);

  if (ctx->device) {
    _destroy_swapchain(ctx);
    _destroy_external_targets(ctx);

    if (ctx->render_pass)
      vkDestroyRenderPass(ctx->device, ctx->render_pass, NULL);

    vkDestroyDevice(ctx->device, NULL);
  }

  if (ctx->surface)
    vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);

  if (ctx->instance)
    vkDestroyInstance(ctx->instance, NULL);

  alloc->free(ctx, alloc->ctx);
  return false;
}

void rn_vk_backend_fini(struct rn_backend_impl_t *backend,
                        const struct rn_allocator_t *alloc) {
  if (!backend || !alloc || backend->create_buffer != _create_buffer)
    return;
  struct rn_vk_context *ctx = backend->ctx;
  if (!ctx)
    return;

  /* Runara owns submission, so it also waits for GPU completion on teardown. */
  if (ctx->device)
    _result(ctx, vkDeviceWaitIdle(ctx->device), "vkDeviceWaitIdle(shutdown)");
  ctx->tearing_down = true;
  _VK_TRACE(
      ctx->log,
      "destroying Vulkan resources (%zu buffers, %zu textures, %zu pipelines)",
      ctx->buffer_count, ctx->texture_count, ctx->pipeline_count);
  for (size_t i = 0; i < ctx->pipeline_count; ++i)
    if (ctx->pipelines[i].alive)
      _destroy_pipeline(ctx, (rn_pipeline_handle_t)i);
  for (size_t i = 0; i < ctx->texture_count; ++i)
    if (ctx->textures[i].alive)
      _destroy_texture(ctx, (rn_texture_handle_t)i);
  for (size_t i = 0; i < ctx->buffer_count; ++i)
    if (ctx->buffers[i].alive)
      _destroy_buffer(ctx, (rn_buffer_handle_t)i);

  if (ctx->upload_fence)
    vkDestroyFence(ctx->device, ctx->upload_fence, NULL);
  if (ctx->upload_pool)
    vkDestroyCommandPool(ctx->device, ctx->upload_pool, NULL);
  if (ctx->vma)
    vmaDestroyAllocator(ctx->vma);
  if (ctx->frame_fence)
    vkDestroyFence(ctx->device, ctx->frame_fence, NULL);
  if (ctx->image_available)
    vkDestroySemaphore(ctx->device, ctx->image_available, NULL);
  if (ctx->frame_pool)
    vkDestroyCommandPool(ctx->device, ctx->frame_pool, NULL);
  _destroy_swapchain(ctx);
  _destroy_external_targets(ctx);
  if (ctx->render_pass)
    vkDestroyRenderPass(ctx->device, ctx->render_pass, NULL);
  vkDestroyDevice(ctx->device, NULL);
  if (ctx->surface)
    vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
  vkDestroyInstance(ctx->instance, NULL);

  alloc->free(ctx->buffers, alloc->ctx);
  alloc->free(ctx->textures, alloc->ctx);
  alloc->free(ctx->pipelines, alloc->ctx);
  _VK_TRACE(ctx->log, "Vulkan backend teardown complete");
  alloc->free(ctx, alloc->ctx);
  memset(backend, 0, sizeof(*backend));
}

static struct rn_vk_context *_state_context(struct rn_state_t *state) {
  if (!state || state->backend.create_buffer != _create_buffer)
    return NULL;

  return state->backend.ctx;
}

rn_result_t
rn_vk_get_native_handles(struct rn_state_t *state,
                         struct rn_vk_native_handles_t *out_handles) {
  struct rn_vk_context *ctx = _state_context(state);
  if (!ctx || !out_handles)
    return RN_ERROR_INVALID_ARGUMENT;

  *out_handles = (struct rn_vk_native_handles_t){
      .instance = ctx->instance,
      .physical_device = ctx->physical_device,
      .device = ctx->device,
      .graphics_queue = ctx->queue,
      .graphics_queue_family = ctx->queue_family,
  };

  return RN_OK;
}

rn_result_t
rn_vk_target_register(struct rn_state_t *state,
                      const struct rn_vk_external_target_desc_t *desc,
                      rn_vk_target_handle_t *out_target) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || ctx->mode != RN_VK_TARGET_EXTERNAL || ctx->frame_active ||
      !desc || !out_target || !desc->image || !desc->width || !desc->height ||
      desc->format != ctx->swap_format)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_vk_external_target *targets = _grow(
      ctx, ctx->external_targets, ctx->external_target_count, sizeof(*targets));

  if (!targets)
    return RN_ERROR_OUT_OF_MEMORY;

  ctx->external_targets = targets;
  struct rn_vk_external_target target = {0};
  target.image = desc->image;
  target.extent = (VkExtent2D){desc->width, desc->height};

  const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = target.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = desc->format,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .levelCount = 1,
              .layerCount = 1,
          },
  };

  /* create image view for the target (using the externally-owned VkImage)*/
  VkResult status =
      vkCreateImageView(ctx->device, &view_info, NULL, &target.view);

  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkCreateImageView(external target)");

  const VkFramebufferCreateInfo fb_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = ctx->render_pass,
      .attachmentCount = 1,
      .pAttachments = &target.view,
      .width = desc->width,
      .height = desc->height,
      .layers = 1,
  };

  /* create framebuffer for the target */ 
  status =
      vkCreateFramebuffer(ctx->device, &fb_info, NULL, &target.framebuffer);

  if (status != VK_SUCCESS) {
    vkDestroyImageView(ctx->device, target.view, NULL);
    return _result(ctx, status, "vkCreateFramebuffer(external target)");
  }

  target.alive = true;
  *out_target = (rn_vk_target_handle_t)ctx->external_target_count;
  ctx->external_targets[ctx->external_target_count++] = target;

  _VK_TRACE(ctx->log, "registered external target %u (%ux%u)", *out_target,
            desc->width, desc->height);

  return RN_OK;
}

rn_result_t rn_vk_target_set(struct rn_state_t *state,
                             rn_vk_target_handle_t handle) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || ctx->mode != RN_VK_TARGET_EXTERNAL || ctx->frame_active ||
      !_external_target(ctx, handle))
    return RN_ERROR_INVALID_ARGUMENT;

  /* set active target */ 
  ctx->active_target = handle;
  return RN_OK;
}

rn_result_t rn_vk_wait_for_render(struct rn_state_t *state) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || ctx->frame_active)
    return RN_ERROR_BAD_STATE;

  if (!ctx->frame_submitted)
    return RN_OK;

  return _result(
      ctx,
      vkWaitForFences(ctx->device, 1, &ctx->frame_fence, VK_TRUE, UINT64_MAX),
      "vkWaitForFences(render complete)");
}

rn_result_t rn_vk_target_unregister(struct rn_state_t *state,
                                    rn_vk_target_handle_t handle) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || ctx->mode != RN_VK_TARGET_EXTERNAL || ctx->frame_active)
    return RN_ERROR_BAD_STATE;

  struct rn_vk_external_target *target = _external_target(ctx, handle);
  if (!target)
    return RN_ERROR_INVALID_ARGUMENT;

  rn_result_t result = _result(ctx, vkDeviceWaitIdle(ctx->device),
                               "vkDeviceWaitIdle(unregister target)");

  if (result != RN_OK)
    return result;

  vkDestroyFramebuffer(ctx->device, target->framebuffer, NULL);
  vkDestroyImageView(ctx->device, target->view, NULL);
  memset(target, 0, sizeof(*target));

  if (ctx->active_target == handle)
    ctx->active_target = RN_VK_INVALID_TARGET;

  return RN_OK;
}

rn_result_t rn_vk_image_from_image(struct rn_state_t *state, VkImage image,
                                     VkFormat format, uint32_t width,
                                     uint32_t height,
                                     struct rn_image_t *out_image) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || !out_image || !image || !width || !height || ctx->frame_active)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_vk_texture *textures =
      _grow(ctx, ctx->textures, ctx->texture_count, sizeof(*textures));

  if (!textures)
    return RN_ERROR_OUT_OF_MEMORY;

  ctx->textures = textures;
  struct rn_vk_texture texture = {0};
  texture.image = image;
  texture.width = width;
  texture.height = height;
  texture.layers = 1;
  texture.borrowed = true;

  const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .levelCount = 1,
              .layerCount = 1,
          },
  };

  VkResult status =
      vkCreateImageView(ctx->device, &view_info, NULL, &texture.view);

  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkCreateImageView(client texture)");

  const VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
      .maxAnisotropy = 1.0f,
  };

  status = vkCreateSampler(ctx->device, &sampler_info, NULL, &texture.sampler);

  if (status != VK_SUCCESS) {
    vkDestroyImageView(ctx->device, texture.view, NULL);
    return _result(ctx, status, "vkCreateSampler(client texture)");
  }

  texture.alive = true;
  rn_texture_handle_t handle = (rn_texture_handle_t)ctx->texture_count;
  ctx->textures[ctx->texture_count++] = texture;

  *out_image = (struct rn_image_t){
      .storage = RN_IMAGE_DEDICATED,
      .texture = handle,
      .width = width,
      .height = height,
      .layer = 0,
      .uv = {0.0f, 0.0f, 1.0f, 1.0f},
  };

  return RN_OK;
}
