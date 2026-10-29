/*
 * Copyright (c) 2026 Luca Machiedo
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _POSIX_C_SOURCE 200809L

#include "src/backend.h"
#include "runara/runara.h"
#include "runara/vulkan.h"
#include "shaders.h"
#include "src/internal.h"
#include "src/log.h"
#include <vulkan/vulkan_core.h>

#define VMA_VULKAN_VERSION 1000000
#include <limits.h>
#include <shaderc/shaderc.h>
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
  void *mapped;
  VkDeviceSize size;
  VkDeviceSize frame_stride;
  enum rn_buffer_target_t target;
  bool alive;
};

struct rn_vk_conversion {
  VkSamplerYcbcrConversion conversion;
  VkSampler sampler;
  VkSamplerYcbcrConversionCreateInfo info;
  uint32_t descriptors;
  VkFormatFeatureFlags features;
  struct rn_vk_conversion *next;
};

struct rn_vk_texture {
  VkImage image;
  VkImageView view;
  VkSampler sampler;
  struct rn_vk_conversion *conversion;
  uint32_t packed_yuv;
  VmaAllocation allocation;
  uint32_t width, height, layers;
  bool array;
  bool borrowed;
  bool alive;
  bool dmabuf, acquired;
  VkDeviceMemory dmabuf_memory[RN_VK_DMABUF_MAX_PLANES];
  int dmabuf_fd[RN_VK_DMABUF_MAX_PLANES];
  uint32_t dmabuf_memory_count;
  int acquire_fd;
  uint64_t dmabuf_device, dmabuf_inode;
  struct rn_vk_dmabuf_desc_t dmabuf_desc;
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

struct rn_vk_descriptor_cache {
  struct rn_vk_descriptor_page *pages;
  struct rn_vk_descriptor_entry *entries;
  struct rn_vk_descriptor_entry *unused;
};

struct rn_vk_pipeline {
  VkPipeline pipeline;
  VkPipelineLayout layout;
  VkDescriptorSetLayout set_layout;
  struct rn_vk_descriptor_cache descriptors[RN_VK_FRAMES_IN_FLIGHT];
  enum rn_shader_pipeline_t shader;
  uint32_t descriptor_count;
  uint32_t strides[RN_VK_MAX_BINDINGS];
  bool alive;
  rn_pipeline_handle_t parent;
  VkSampler immutable[RN_VK_MAX_TEXTURES];
  uint32_t pool_descriptors;
  struct rn_pipeline_desc_t source;
  struct rn_vertex_buffer_layout_t source_bindings[RN_VK_MAX_BINDINGS];
  struct rn_vertex_attribute_t source_attributes[32];
};

struct rn_vk_external_target {
  VkImage image;
  VkImageView view;
  VkFramebuffer framebuffer;
  VkExtent2D extent;
  VkFence render_fence;
  bool alive;
  bool used;
};

struct rn_vk_swap_image {
  VkImageView view;
  VkFramebuffer framebuffer;
  VkSemaphore render_done;
};

struct rn_vk_managed_surface_t {
  VkSurfaceKHR vk_surface;
  VkSwapchainKHR swapchain;
  VkQueue present_queue;
  uint32_t present_family;
  VkExtent2D swap_extent;
  struct rn_vk_swap_image *swap_images;
  uint32_t swap_image_count;
  uint32_t image_index;
  uint32_t requested_width, requested_height;
  bool resize_pending;
  struct rn_vk_surface_impl_t impl;
  struct rn_vk_managed_surface_t *next;
};

struct rn_vk_frame {
  VkCommandPool pool;
  VkCommandBuffer command;
  VkSemaphore image_available;
  VkFence fence;
  uint64_t submission;
  VkSemaphore *acquire_semaphores, *wait_semaphores;
  VkPipelineStageFlags *wait_stages;
  uint32_t acquire_capacity, wait_count;
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

  /* created surfaces and the surface currently rendered to */
  struct rn_vk_managed_surface_t *current_surface;
  struct rn_vk_managed_surface_t *surfaces;

  uint32_t queue_family_count;
  VkFormat swap_format;
  bool fatal;
  bool tearing_down;

  struct rn_vk_frame frames[RN_VK_FRAMES_IN_FLIGHT];
  uint32_t frame_index;
  uint64_t submitted, completed;

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
  VkBuffer staging;
  VmaAllocation staging_allocation;
  void *staging_map;
  size_t staging_capacity;
  bool dmabuf_enabled;
  bool ycbcr_enabled;
  bool ycbcr_444_enabled;
  uint32_t texture_slots;
  struct rn_vk_conversion *conversions;
  VkSampler immutable_rgb;
  PFN_vkCreateSamplerYcbcrConversionKHR create_conversion;
  PFN_vkDestroySamplerYcbcrConversionKHR destroy_conversion;
  struct rn_vk_dmabuf_format_t *dmabuf_formats;
  size_t dmabuf_format_count;
  rn_texture_handle_t *acquired_images;
  size_t acquired_count, acquired_capacity;
  PFN_vkGetPhysicalDeviceFormatProperties2KHR format_properties;
  PFN_vkGetPhysicalDeviceImageFormatProperties2KHR image_properties;
  PFN_vkGetMemoryFdPropertiesKHR fd_properties;
  PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
  VkPhysicalDeviceProperties properties;

  struct rn_vk_buffer *buffers;
  size_t buffer_count;
  struct rn_vk_texture *textures;
  size_t texture_count;
  struct rn_vk_pipeline *pipelines;
  size_t pipeline_count;

  rn_pipeline_handle_t pipeline;
  VkPipeline bound_pipeline;
  rn_buffer_handle_t vertex_buffers[RN_VK_MAX_BINDINGS];
  rn_buffer_handle_t index_buffer;
  rn_texture_handle_t bound_textures[RN_VK_MAX_TEXTURES];
  uint32_t extent_x, extent_y, extent_width, extent_height;
  bool frame_active;

  const struct rn_allocator_t *alloc;
  const struct rn_log_t *log;

  const char *frag_dedicated;
  const char *frag_atlas;
  const char *vert_src;
};

static rn_result_t _dmabuf_init(struct rn_vk_context *ctx);
static rn_result_t _dmabuf_release_pending(struct rn_vk_context *ctx);
static rn_result_t _dmabuf_release_texture(struct rn_vk_context *ctx,
                                           struct rn_vk_texture *texture);
static void _dmabuf_release_commands(struct rn_vk_context *ctx,
                                     VkCommandBuffer command);
static rn_result_t _dmabuf_acquire_commands(struct rn_vk_context *ctx,
                                            struct rn_vk_frame *frame);
static void _dmabuf_forget(struct rn_vk_context *ctx,
                           rn_texture_handle_t handle);
static void _dmabuf_close(struct rn_vk_texture *texture);
static rn_result_t _write_texture(void *backend_ctx,
                                  const struct rn_texture_array_write_t *write,
                                  const void *pixels, size_t size);

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
static rn_result_t _clear_color_buffer(void *backend_ctx);
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

rn_result_t _compile_shader(struct rn_vk_context *ctx, const char *glsl,
                            VkShaderStageFlagBits stage, uint32_t **spv,
                            size_t *spv_size);

static const struct rn_backend_impl_t _rn_vk_dispatch = {
    .begin_frame = _begin_frame,
    .clear_color = _clear_color,
    .clear_color_buffer = _clear_color_buffer,
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
    .write_texture = _write_texture,
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

static rn_texture_handle_t _texture_slot(struct rn_vk_context *ctx) {
  for (size_t i = 0; i < ctx->texture_count; ++i) {
    if (!ctx->textures[i].alive)
      return (rn_texture_handle_t)i;
  }

  struct rn_vk_texture *textures =
      _grow(ctx, ctx->textures, ctx->texture_count, sizeof(*textures));

  if (!textures)
    return RN_INVALID_HANDLE;

  ctx->textures = textures;
  memset(&textures[ctx->texture_count], 0, sizeof(*textures));
  return (rn_texture_handle_t)ctx->texture_count++;
}

static struct rn_vk_pipeline *_pipeline(struct rn_vk_context *ctx,
                                        rn_pipeline_handle_t handle) {
  return handle < ctx->pipeline_count && ctx->pipelines[handle].alive
             ? &ctx->pipelines[handle]
             : NULL;
}

static rn_result_t _create_instance(struct rn_vk_context *ctx) {
  const struct rn_vk_init_options_t *options = &ctx->config;
  uint32_t count = 0;
  const char *const *required = NULL;

  if (options->instance_extensions)
    required = options->instance_extensions(options->user, &count);

  if (count > 30 || (count && !required))
    return RN_ERROR_INVALID_ARGUMENT;

  const char *extensions[32] = {0};

  uint32_t extension_count = 0;

  if (ctx->mode == RN_VK_TARGET_MANAGED)
    extensions[extension_count++] = VK_KHR_SURFACE_EXTENSION_NAME;

#ifdef __linux__
  if (ctx->config.enable_dmabuf_import) {
    extensions[extension_count++] =
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    extensions[extension_count++] =
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;
    extensions[extension_count++] =
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME;
  }
#endif

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

    if (!found) {
      if (extension_count == sizeof(extensions) / sizeof(extensions[0]))
        return RN_ERROR_INVALID_ARGUMENT;

      extensions[extension_count++] = required[i];
    }
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

    if (ctx->config.accept_device &&
        !ctx->config.accept_device(ctx->config.user, ctx->instance, gpu))
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

    for (uint32_t j = 0; j < queue_count; ++j) {

      if (queues[j].queueCount &&
          (queues[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        graphics = j;
        break;
      }
    }

    ctx->alloc->free(queues, ctx->alloc->ctx);

    if (graphics == UINT32_MAX)
      continue;

    ctx->physical_device = gpu;
    ctx->queue_family = graphics;
    /* later surfaces may need a presentation queue from another family */
    ctx->queue_family_count = queue_count;
    ctx->properties = props;
    break;
  }

  ctx->alloc->free(devices, ctx->alloc->ctx);

  if (!ctx->physical_device) {
    _VK_ERROR(ctx->log, "no compatible Vulkan 1.0 physical device");
    return RN_ERROR_BACKEND;
  }

#ifdef __linux__
  static const char *const dmabuf_extensions[] = {
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
      VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
      VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
      VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
      VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
      VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
  };
  const uint32_t dmabuf_extension_count =
      sizeof(dmabuf_extensions) / sizeof(dmabuf_extensions[0]);

  if (ctx->config.enable_dmabuf_import &&
      _has_device_extensions(ctx, ctx->physical_device, dmabuf_extensions,
                             dmabuf_extension_count)) {
    for (uint32_t i = 0; i < dmabuf_extension_count; ++i) {
      bool found = false;

      for (uint32_t j = 0; j < extension_count; ++j)
        found |= strcmp(extensions[j], dmabuf_extensions[i]) == 0;

      if (!found) {
        if (extension_count == sizeof(extensions) / sizeof(extensions[0]))
          return RN_ERROR_INVALID_ARGUMENT;

        extensions[extension_count++] = dmabuf_extensions[i];
      }
    }

    ctx->dmabuf_enabled = true;
  }
#endif

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo *queues = ctx->alloc->alloc(
      ctx->queue_family_count * sizeof(*queues), ctx->alloc->ctx);

  if (!queues)
    return RN_ERROR_OUT_OF_MEMORY;

  for (uint32_t i = 0; i < ctx->queue_family_count; ++i) {

    queues[i] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = i,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
  }

  VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT ycbcr444 = {
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_2_PLANE_444_FORMATS_FEATURES_EXT};
  const char *extension444 = VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME;
  bool has444 =
      ctx->dmabuf_enabled &&
      _has_device_extensions(ctx, ctx->physical_device, &extension444, 1);
  bool added444 = false;
  for (uint32_t i = 0; i < extension_count; ++i)
    added444 |= !strcmp(extensions[i], extension444);
  if (has444 && !added444) {
    if (extension_count == sizeof(extensions) / sizeof(extensions[0]))
      return RN_ERROR_INVALID_ARGUMENT;
    extensions[extension_count++] = extension444;
  }
  VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
      .pNext = has444 ? &ycbcr444 : NULL,
  };
  if (ctx->dmabuf_enabled) {
    PFN_vkGetPhysicalDeviceFeatures2KHR get_features =
        (PFN_vkGetPhysicalDeviceFeatures2KHR)vkGetInstanceProcAddr(
            ctx->instance, "vkGetPhysicalDeviceFeatures2KHR");
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ycbcr};
    if (get_features)
      get_features(ctx->physical_device, &features);
    ctx->ycbcr_enabled = ycbcr.samplerYcbcrConversion;
    ctx->ycbcr_444_enabled = ycbcr444.ycbcr2plane444Formats;
  }

  const VkDeviceCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = ctx->dmabuf_enabled ? &ycbcr : NULL,
      .queueCreateInfoCount = ctx->queue_family_count,
      .pQueueCreateInfos = queues,
      .enabledExtensionCount = extension_count,
      .ppEnabledExtensionNames = extensions,
  };

  rn_result_t result = _result(
      ctx, vkCreateDevice(ctx->physical_device, &info, NULL, &ctx->device),
      "vkCreateDevice");

  ctx->alloc->free(queues, ctx->alloc->ctx);

  if (result != RN_OK)
    return result;

  vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);
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
  struct rn_vk_managed_surface_t *surface = ctx->current_surface;

  for (uint32_t i = 0; i < surface->swap_image_count; ++i) {
    struct rn_vk_swap_image *image = &surface->swap_images[i];

    if (image->render_done)
      vkDestroySemaphore(ctx->device, image->render_done, NULL);

    if (image->framebuffer)
      vkDestroyFramebuffer(ctx->device, image->framebuffer, NULL);

    if (image->view)
      vkDestroyImageView(ctx->device, image->view, NULL);
  }

  if (surface->swap_images)
    ctx->alloc->free(surface->swap_images, ctx->alloc->ctx);
  surface->swap_images = NULL;
  surface->swap_image_count = 0;

  if (surface->swapchain)
    vkDestroySwapchainKHR(ctx->device, surface->swapchain, NULL);
  surface->swapchain = VK_NULL_HANDLE;
}

static rn_result_t _create_swapchain(struct rn_vk_context *ctx,
                                     uint32_t requested_width,
                                     uint32_t requested_height) {
  struct rn_vk_managed_surface_t *surface = ctx->current_surface;

  if (!requested_width || !requested_height)
    return RN_ERROR_BAD_STATE;

  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

  if (surface->impl.disable_vsync) {
    uint32_t count = 0;
    VkResult status = vkGetPhysicalDeviceSurfacePresentModesKHR(
        ctx->physical_device, surface->vk_surface, &count, NULL);

    if (status != VK_SUCCESS)
      return _result(ctx, status, "vkGetPhysicalDeviceSurfacePresentModesKHR");

    if (!count)
      return RN_ERROR_BACKEND;

    VkPresentModeKHR *modes =
        ctx->alloc->alloc(count * sizeof(*modes), ctx->alloc->ctx);

    if (!modes)
      return RN_ERROR_OUT_OF_MEMORY;

    status = vkGetPhysicalDeviceSurfacePresentModesKHR(
        ctx->physical_device, surface->vk_surface, &count, modes);

    if (status == VK_SUCCESS) {

      for (uint32_t i = 0; i < count; ++i) {

        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
          present_mode = modes[i];
          break;
        }
      }
    }

    ctx->alloc->free(modes, ctx->alloc->ctx);

    if (status != VK_SUCCESS)
      return _result(ctx, status, "vkGetPhysicalDeviceSurfacePresentModesKHR");

    if (present_mode != VK_PRESENT_MODE_IMMEDIATE_KHR &&
        !surface->impl.allow_vsync_fallback) {
      _VK_ERROR(ctx->log,
                "surface does not support presentation without vsync");
      return RN_ERROR_BACKEND;
    }
  }

  VkSurfaceCapabilitiesKHR capabilities;
  VkResult status = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      ctx->physical_device, surface->vk_surface, &capabilities);

  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

  /* the surface capabilities changed to not support color attachment usage,
   * we cannot create a swapchain. */

  if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    return RN_ERROR_BACKEND;

  /* Get surface formats (count) */
  uint32_t format_count = 0;
  status = vkGetPhysicalDeviceSurfaceFormatsKHR(
      ctx->physical_device, surface->vk_surface, &format_count, NULL);

  if (status != VK_SUCCESS || !format_count)
    return RN_ERROR_BACKEND;

  VkSurfaceFormatKHR *formats =
      ctx->alloc->alloc(format_count * sizeof(*formats), ctx->alloc->ctx);

  if (!formats)
    return RN_ERROR_OUT_OF_MEMORY;

  /* Get surface formats (data) */
  status = vkGetPhysicalDeviceSurfaceFormatsKHR(
      ctx->physical_device, surface->vk_surface, &format_count, formats);

  if (status != VK_SUCCESS) {
    ctx->alloc->free(formats, ctx->alloc->ctx);
    return _result(ctx, status, "vkGetPhysicalDeviceSurfaceFormatsKHR");
  }

  /* all surfaces share the render pass and graphics pipelines */

  VkSurfaceFormatKHR chosen = {0};

  bool found = false;

  for (uint32_t i = 0; i < format_count; ++i) {

    if (formats[i].format == ctx->swap_format ||
        (format_count == 1 && formats[i].format == VK_FORMAT_UNDEFINED)) {
      chosen = formats[i];
      chosen.format = ctx->swap_format;
      found = true;
      break;
    }
  }

  ctx->alloc->free(formats, ctx->alloc->ctx);

  if (!found) {
    _VK_ERROR(ctx->log, "surface does not support the shared render format");
    return RN_ERROR_BACKEND;
  }

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

  uint32_t image_count = capabilities.minImageCount + 1;

  if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
    image_count = capabilities.maxImageCount;

  const uint32_t families[2] = {ctx->queue_family, surface->present_family};

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
      .surface = surface->vk_surface,
      .minImageCount = image_count,
      .imageFormat = ctx->swap_format,
      .imageColorSpace = chosen.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = ctx->queue_family == surface->present_family
                              ? VK_SHARING_MODE_EXCLUSIVE
                              : VK_SHARING_MODE_CONCURRENT,
      .queueFamilyIndexCount =
          ctx->queue_family == surface->present_family ? 0u : 2u,
      .pQueueFamilyIndices =
          ctx->queue_family == surface->present_family ? NULL : families,
      .preTransform = capabilities.currentTransform,
      .compositeAlpha = alpha,
      .presentMode = present_mode,
      .clipped = VK_TRUE,
  };

  /* Called only after vkDeviceWaitIdle, which is called before, by the caller
   */
  _destroy_swapchain(ctx);

  /* create new swapchain */
  status = vkCreateSwapchainKHR(ctx->device, &info, NULL, &surface->swapchain);

  if (status != VK_SUCCESS)
    return _result(ctx, status, "vkCreateSwapchainKHR");

  /* Get new swapchain images, to allocate vkImages, vkImageViews and eventually
   * framebuffers from */
  status = vkGetSwapchainImagesKHR(ctx->device, surface->swapchain,
                                   &image_count, NULL);

  if (status != VK_SUCCESS || !image_count)
    return RN_ERROR_BACKEND;

  /* allocate temp images */
  VkImage *images =
      ctx->alloc->alloc(image_count * sizeof(*images), ctx->alloc->ctx);

  if (!images)
    return RN_ERROR_OUT_OF_MEMORY;

  status = vkGetSwapchainImagesKHR(ctx->device, surface->swapchain,
                                   &image_count, images);

  if (status != VK_SUCCESS) {
    ctx->alloc->free(images, ctx->alloc->ctx);
    return _result(ctx, status, "vkGetSwapchainImagesKHR");
  }

  /* allocate retained swapchain images */
  surface->swap_images = ctx->alloc->alloc(
      image_count * sizeof(*surface->swap_images), ctx->alloc->ctx);

  if (!surface->swap_images) {
    /* destroy temp images */
    ctx->alloc->free(images, ctx->alloc->ctx);
    return RN_ERROR_OUT_OF_MEMORY;
  }

  memset(surface->swap_images, 0, image_count * sizeof(*surface->swap_images));
  surface->swap_image_count = image_count;

  for (uint32_t i = 0; i < image_count; ++i) {
    struct rn_vk_swap_image *target = &surface->swap_images[i];

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

  surface->swap_extent = extent;
  surface->requested_width = requested_width;
  surface->requested_height = requested_height;
  surface->resize_pending = false;
  _VK_TRACE(ctx->log, "created swapchain (%ux%u, %u images)", extent.width,
            extent.height, image_count);
  return RN_OK;
}

static void _destroy_frame_commands(struct rn_vk_context *ctx) {

  for (uint32_t i = 0; i < RN_VK_FRAMES_IN_FLIGHT; ++i) {
    struct rn_vk_frame *frame = &ctx->frames[i];

    for (uint32_t j = 0; j < frame->acquire_capacity; ++j)
      if (frame->acquire_semaphores[j])
        vkDestroySemaphore(ctx->device, frame->acquire_semaphores[j], NULL);
    ctx->alloc->free(frame->acquire_semaphores, ctx->alloc->ctx);
    ctx->alloc->free(frame->wait_semaphores, ctx->alloc->ctx);
    ctx->alloc->free(frame->wait_stages, ctx->alloc->ctx);

    if (frame->fence)
      vkDestroyFence(ctx->device, frame->fence, NULL);

    if (frame->image_available)
      vkDestroySemaphore(ctx->device, frame->image_available, NULL);

    if (frame->pool)
      vkDestroyCommandPool(ctx->device, frame->pool, NULL);
  }
}

static rn_result_t _create_frame_commands(struct rn_vk_context *ctx) {

  const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = ctx->queue_family,
  };

  const VkSemaphoreCreateInfo semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
  };

  const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
  };

  for (uint32_t i = 0; i < RN_VK_FRAMES_IN_FLIGHT; ++i) {
    struct rn_vk_frame *frame = &ctx->frames[i];
    rn_result_t result = _result(
        ctx, vkCreateCommandPool(ctx->device, &pool_info, NULL, &frame->pool),
        "vkCreateCommandPool(frame)");

    if (result != RN_OK)
      return result;

    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = frame->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    result = _result(
        ctx,
        vkAllocateCommandBuffers(ctx->device, &command_info, &frame->command),
        "vkAllocateCommandBuffers(frame)");

    if (result != RN_OK)
      return result;

    if (ctx->mode == RN_VK_TARGET_MANAGED) {
      result = _result(ctx,
                       vkCreateSemaphore(ctx->device, &semaphore_info, NULL,
                                         &frame->image_available),
                       "vkCreateSemaphore(image available)");

      if (result != RN_OK)
        return result;
    }

    result = _result(
        ctx, vkCreateFence(ctx->device, &fence_info, NULL, &frame->fence),
        "vkCreateFence(frame)");

    if (result != RN_OK)
      return result;
  }

  return RN_OK;
}

static rn_result_t _prepare_frame_waits(struct rn_vk_context *ctx,
                                        struct rn_vk_frame *frame) {
  if (ctx->acquired_count > UINT32_MAX - 1)
    return RN_ERROR_OUT_OF_MEMORY;
  if (!frame->wait_semaphores ||
      ctx->acquired_count > frame->acquire_capacity) {
    uint32_t capacity = frame->acquire_capacity ? frame->acquire_capacity : 8;
    while (capacity < ctx->acquired_count) {
      if (capacity > (UINT32_MAX - 1) / 2)
        return RN_ERROR_OUT_OF_MEMORY;
      capacity *= 2;
    }
    VkSemaphore *acquire =
        ctx->alloc->alloc((size_t)capacity * sizeof(*acquire), ctx->alloc->ctx);
    VkSemaphore *wait = ctx->alloc->alloc(
        ((size_t)capacity + 1) * sizeof(*wait), ctx->alloc->ctx);
    VkPipelineStageFlags *stages = ctx->alloc->alloc(
        ((size_t)capacity + 1) * sizeof(*stages), ctx->alloc->ctx);
    if (!acquire || !wait || !stages) {
      ctx->alloc->free(acquire, ctx->alloc->ctx);
      ctx->alloc->free(wait, ctx->alloc->ctx);
      ctx->alloc->free(stages, ctx->alloc->ctx);
      return RN_ERROR_OUT_OF_MEMORY;
    }
    memset(acquire, 0, (size_t)capacity * sizeof(*acquire));
    if (frame->acquire_capacity)
      memcpy(acquire, frame->acquire_semaphores,
             (size_t)frame->acquire_capacity * sizeof(*acquire));
    ctx->alloc->free(frame->acquire_semaphores, ctx->alloc->ctx);
    ctx->alloc->free(frame->wait_semaphores, ctx->alloc->ctx);
    ctx->alloc->free(frame->wait_stages, ctx->alloc->ctx);
    frame->acquire_semaphores = acquire;
    frame->wait_semaphores = wait;
    frame->wait_stages = stages;
    frame->acquire_capacity = capacity;
  }
  frame->wait_count = 0;
  if (ctx->mode == RN_VK_TARGET_MANAGED) {
    frame->wait_semaphores[frame->wait_count] = frame->image_available;
    frame->wait_stages[frame->wait_count++] =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  }
  return RN_OK;
}

static rn_result_t _begin_frame(void *backend_ctx, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height) {
  struct rn_vk_context *ctx = backend_ctx;
  struct rn_vk_managed_surface_t *surface = ctx ? ctx->current_surface : NULL;

  if (!ctx || ctx->frame_active || ctx->fatal ||
      (ctx->mode == RN_VK_TARGET_MANAGED && !surface) || !width || !height)
    return RN_ERROR_BAD_STATE;

  struct rn_vk_frame *frame = &ctx->frames[ctx->frame_index];

  rn_result_t result = _result(
      ctx, vkWaitForFences(ctx->device, 1, &frame->fence, VK_TRUE, UINT64_MAX),
      "vkWaitForFences(frame)");

  if (result != RN_OK)
    return result;

  if (frame->submission > ctx->completed)
    ctx->completed = frame->submission;

  /* reuse descriptor storage only after the frame slot's fence has signaled */
  for (size_t i = 0; i < ctx->pipeline_count; ++i) {
    if (ctx->pipelines[i].alive) {
      struct rn_vk_descriptor_cache *cache =
          &ctx->pipelines[i].descriptors[ctx->frame_index];
      cache->unused = cache->entries;
    }
  }

  VkFramebuffer framebuffer = VK_NULL_HANDLE;

  VkExtent2D extent = {0};

  struct rn_vk_external_target *target = NULL;

  if (ctx->mode == RN_VK_TARGET_MANAGED) {
    uint32_t framebuffer_width = width;
    uint32_t framebuffer_height = height;

    surface->impl.framebuffer_size(surface->impl.user, &framebuffer_width,
                                   &framebuffer_height);

    if (!framebuffer_width || !framebuffer_height)
      return RN_ERROR_BAD_STATE;

    /* pending resizes need to wait for idle */

    if (surface->resize_pending ||
        framebuffer_width != surface->requested_width ||
        framebuffer_height != surface->requested_height) {
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
      status = vkAcquireNextImageKHR(ctx->device, surface->swapchain,
                                     UINT64_MAX, frame->image_available,
                                     VK_NULL_HANDLE, &surface->image_index);

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
      surface->resize_pending = true;

    /* get the framebuffer of the swapchain image at the current, returned
     * index. */
    framebuffer = surface->swap_images[surface->image_index].framebuffer;
    extent = surface->swap_extent;

  } else {
    target = _external_target(ctx, ctx->active_target);

    if (!target)
      return RN_ERROR_BAD_STATE;

    /* a borrowed target cannot be rendered again while its prior use is pending
     */
    if (target->render_fence && target->render_fence != frame->fence) {
      result = _result(ctx,
                       vkWaitForFences(ctx->device, 1, &target->render_fence,
                                       VK_TRUE, UINT64_MAX),
                       "vkWaitForFences(external target)");

      if (result != RN_OK)
        return result;
    }

    framebuffer = target->framebuffer;
    extent = target->extent;
  }

  result = _result(ctx, vkResetCommandPool(ctx->device, frame->pool, 0),
                   "vkResetCommandPool(frame)");

  if (result != RN_OK)
    return result;

  /* command buffer use is began for both managed and external mode */

  const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };

  result = _result(ctx, vkBeginCommandBuffer(frame->command, &begin),
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

    vkCmdPipelineBarrier(frame->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, 1, &acquire);
  }

  result = _prepare_frame_waits(ctx, frame);
  if (result != RN_OK)
    return result;
  result = _dmabuf_acquire_commands(ctx, frame);
  if (result != RN_OK)
    return result;

  /* render pass is began for both managed and external mode */

  const VkRenderPassBeginInfo pass = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = ctx->render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, extent},
  };

  vkCmdBeginRenderPass(frame->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

  ctx->command_buffer = frame->command;
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

  vkCmdSetViewport(frame->command, 0, 1, &viewport);
  vkCmdSetScissor(frame->command, 0, 1, &scissor);

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

static rn_result_t _clear_color_buffer(void *backend_ctx) {
  struct rn_vk_context *ctx = backend_ctx;

  if (!ctx || !ctx->frame_active)
    return RN_ERROR_BAD_STATE;

  const VkClearAttachment attachment = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .colorAttachment = 0,
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
  struct rn_vk_managed_surface_t *surface = ctx ? ctx->current_surface : NULL;

  if (!ctx || !ctx->frame_active)
    return RN_ERROR_BAD_STATE;

  struct rn_vk_frame *frame = &ctx->frames[ctx->frame_index];

  struct rn_vk_external_target *target = NULL;

  vkCmdEndRenderPass(ctx->command_buffer);
  _dmabuf_release_commands(ctx, ctx->command_buffer);

  if (ctx->mode == RN_VK_TARGET_EXTERNAL) {
    target = _external_target(ctx, ctx->active_target);

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
        frame->command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
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
  result = _result(ctx, vkResetFences(ctx->device, 1, &frame->fence),
                   "vkResetFences(frame)");

  if (result != RN_OK)
    return result;

  VkSemaphore render_done = VK_NULL_HANDLE;

  if (ctx->mode == RN_VK_TARGET_MANAGED)
    render_done = surface->swap_images[surface->image_index].render_done;

  const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = frame->wait_count,
      .pWaitSemaphores = frame->wait_semaphores,
      .pWaitDstStageMask = frame->wait_stages,
      .commandBufferCount = 1,
      .pCommandBuffers = &frame->command,
      .signalSemaphoreCount = ctx->mode == RN_VK_TARGET_MANAGED ? 1u : 0u,
      .pSignalSemaphores =
          ctx->mode == RN_VK_TARGET_MANAGED ? &render_done : NULL,
  };

  VkResult status = vkQueueSubmit(ctx->queue, 1, &submit, frame->fence);

  if (status != VK_SUCCESS) {
    ctx->fatal = true;
    return _result(ctx, status, "vkQueueSubmit(frame)");
  }

  if (target)
    target->render_fence = frame->fence;

  _dmabuf_release_pending(ctx);
  frame->submission = ++ctx->submitted;
  ctx->frame_submitted = true;
  ctx->frame_index = (ctx->frame_index + 1) % RN_VK_FRAMES_IN_FLIGHT;

  if (ctx->mode == RN_VK_TARGET_EXTERNAL)
    return RN_OK;

  const VkPresentInfoKHR present = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &render_done,
      .swapchainCount = 1,
      .pSwapchains = &surface->swapchain,
      .pImageIndices = &surface->image_index,
  };

  status = vkQueuePresentKHR(surface->present_queue, &present);

  if (status == VK_SUBOPTIMAL_KHR || status == VK_ERROR_OUT_OF_DATE_KHR) {
    surface->resize_pending = true;
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

  VkDeviceSize frame_stride = 0;
  VkDeviceSize allocation_size = desc->size;

  if (desc->usage == RN_BUFFER_DYNAMIC) {
    VkDeviceSize alignment = ctx->properties.limits.nonCoherentAtomSize;

    if (alignment < 4)
      alignment = 4;

    if (desc->size > SIZE_MAX - (alignment - 1))
      return RN_ERROR_INVALID_ARGUMENT;

    /* separate noncoherent flush ranges for slots that are still in use */
    frame_stride = (desc->size + alignment - 1) & ~(alignment - 1);

    if (frame_stride > SIZE_MAX / RN_VK_FRAMES_IN_FLIGHT)
      return RN_ERROR_INVALID_ARGUMENT;

    allocation_size = frame_stride * RN_VK_FRAMES_IN_FLIGHT;
  }

  const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = allocation_size,
      .usage = (desc->target == RN_BUFFER_INDEX
                    ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                    : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  const VmaAllocationCreateInfo alloc_info = {
      .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
  };

  struct rn_vk_buffer buffer = {.size = (VkDeviceSize)desc->size,
                                .frame_stride = frame_stride,
                                .target = desc->target};

  VmaAllocationInfo allocation_info;
  VkResult status =
      vmaCreateBuffer(ctx->vma, &buffer_info, &alloc_info, &buffer.buffer,
                      &buffer.allocation, &allocation_info);

  if (status != VK_SUCCESS)
    return _result(ctx, status, "vmaCreateBuffer");

  buffer.mapped = allocation_info.pMappedData;
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

  /* writes outside a frame initialize every region after pending readers finish
   */
  if (ctx->frame_submitted && (!buffer->frame_stride || !ctx->frame_active)) {
    rn_result_t result = _result(ctx, vkDeviceWaitIdle(ctx->device),
                                 "vkDeviceWaitIdle(buffer write)");

    if (result != RN_OK)
      return result;
  }

  uint32_t first =
      buffer->frame_stride && ctx->frame_active ? ctx->frame_index : 0;

  uint32_t count =
      buffer->frame_stride && !ctx->frame_active ? RN_VK_FRAMES_IN_FLIGHT : 1;

  for (uint32_t i = first; i < first + count; ++i) {
    VkDeviceSize write_offset = buffer->frame_stride * i + offset;
    memcpy((unsigned char *)buffer->mapped + write_offset, data, size);
    VkResult status =
        vmaFlushAllocation(ctx->vma, buffer->allocation, write_offset, size);

    if (status != VK_SUCCESS)
      return _result(ctx, status, "vmaFlushAllocation(buffer)");
  }

  return RN_OK;
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

  const VkDeviceSize offset = buffer->frame_stride * ctx->frame_index;
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
  vkCmdBindIndexBuffer(ctx->command_buffer, buffer->buffer,
                       buffer->frame_stride * ctx->frame_index,
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

static rn_result_t
_create_pipeline_samplers(void *backend_ctx,
                          const struct rn_pipeline_desc_t *desc,
                          const VkSampler *samplers, uint32_t pool_descriptors,
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

  /* Atlas uses one combined sampler. Dedicated batches use 16 slots. */
  pipeline.descriptor_count =
      desc->shader == RN_PIPELINE_UI_QUAD_ATLAS ? 1u : ctx->texture_slots;

  const VkDescriptorSetLayoutBinding sampler_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = pipeline.descriptor_count,
      .pImmutableSamplers = samplers,
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

  const VkPushConstantRange push_ranges[2] = {
      {
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
          .offset = 0,
          .size = 8,
      },
      {
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
          .offset = 8,
          .size = ctx->texture_slots * sizeof(uint32_t),
      }};

  const VkPipelineLayoutCreateInfo pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &pipeline.set_layout,
      .pushConstantRangeCount = 2,
      .pPushConstantRanges = push_ranges,
  };

  result = _result(ctx,
                   vkCreatePipelineLayout(ctx->device, &pipeline_layout_info,
                                          NULL, &pipeline.layout),
                   "vkCreatePipelineLayout");

  if (result != RN_OK)
    goto fail;

  VkShaderModule vertex_module = VK_NULL_HANDLE;
  VkShaderModule fragment_module = VK_NULL_HANDLE;

  uint32_t *vertex_spv = (uint32_t *)rn_vk_vertex_spv;
  size_t vertex_spv_size = rn_vk_vertex_spv_size;

  result = _shader_module(ctx, vertex_spv, vertex_spv_size, &vertex_module);

  if (result != RN_OK)
    goto fail;

  uint32_t *frag_spv = desc->shader == RN_PIPELINE_UI_QUAD_ATLAS
                           ? (uint32_t *)rn_vk_atlas_spv
                           : (uint32_t *)rn_vk_dedicated_spv;
  size_t frag_spv_size = desc->shader == RN_PIPELINE_UI_QUAD_ATLAS
                             ? rn_vk_atlas_spv_size
                             : rn_vk_dedicated_spv_size;

  if (desc->shader == RN_PIPELINE_UI_QUAD_DEDICATED) {
    if (ctx->texture_slots == 4) {
      frag_spv = (uint32_t *)rn_vk_dedicated4_spv;
      frag_spv_size = rn_vk_dedicated4_spv_size;
    } else if (ctx->texture_slots == 8) {
      frag_spv = (uint32_t *)rn_vk_dedicated8_spv;
      frag_spv_size = rn_vk_dedicated8_spv_size;
    }
  }

  if (ctx->frag_atlas && desc->shader == RN_PIPELINE_UI_QUAD_ATLAS) {
    result = _compile_shader(ctx, ctx->frag_atlas, VK_SHADER_STAGE_FRAGMENT_BIT,
                             &frag_spv, &frag_spv_size);

    if (result != RN_OK) {
      goto fail;
    }

  } else if (ctx->frag_dedicated &&
             desc->shader == RN_PIPELINE_UI_QUAD_DEDICATED) {
    result =
        _compile_shader(ctx, ctx->frag_dedicated, VK_SHADER_STAGE_FRAGMENT_BIT,
                        &frag_spv, &frag_spv_size);

    if (result != RN_OK) {
      goto fail;
    }
  }

  result = _shader_module(ctx, frag_spv, frag_spv_size, &fragment_module);

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

  pipeline.source = *desc;
  pipeline.source.uniform_binding_count = 0;
  pipeline.source.uniform_bindings = NULL;
  size_t saved_attributes = 0;
  for (size_t i = 0; i < desc->vertex_binding_count; ++i) {
    pipeline.source_bindings[i] = desc->vertex_bindings[i];
    size_t n = desc->vertex_bindings[i].attribute_count;
    if (n)
      memcpy(pipeline.source_attributes + saved_attributes,
             desc->vertex_bindings[i].attributes,
             n * sizeof(*pipeline.source_attributes));
    saved_attributes += n;
  }
  pipeline.pool_descriptors =
      pool_descriptors ? pool_descriptors : pipeline.descriptor_count;
  if (samplers)
    memcpy(pipeline.immutable, samplers,
           pipeline.descriptor_count * sizeof(*samplers));
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

static rn_result_t _create_pipeline(void *ctx,
                                    const struct rn_pipeline_desc_t *desc,
                                    rn_pipeline_handle_t *out) {
  return _create_pipeline_samplers(ctx, desc, NULL, 0, out);
}

/* YCbCr samplers must be immutable. we cache layouts/pipelines by the sampler
 * tuple, so RGB and YUV images can stay in the same zero-copy batch. */
static rn_result_t _sampling_pipeline(struct rn_vk_context *ctx,
                                      struct rn_vk_pipeline **out) {
  struct rn_vk_pipeline *base = *out;
  VkSampler samplers[RN_VK_MAX_TEXTURES] = {0};
  bool converted = false;
  uint32_t descriptors = 0, maximum = 1;
  struct rn_vk_texture *fallback = _texture(ctx, ctx->bound_textures[0]);
  if (!fallback)
    return RN_ERROR_BAD_STATE;
  for (uint32_t i = 0; i < base->descriptor_count; ++i) {
    struct rn_vk_texture *t = _texture(ctx, ctx->bound_textures[i]);
    if (!t)
      t = fallback;
    samplers[i] = ctx->immutable_rgb;
    if (t->conversion) {
      samplers[i] = t->sampler;
      converted = true;
      if (t->conversion->descriptors > maximum)
        maximum = t->conversion->descriptors;
    }
  }
  if (!converted)
    return RN_OK;

  /* all elements of a binding consume the maximum conversion descriptor size */
  descriptors = maximum * base->descriptor_count;

  rn_pipeline_handle_t parent = (rn_pipeline_handle_t)(base - ctx->pipelines);
  for (size_t i = 0; i < ctx->pipeline_count; ++i) {
    struct rn_vk_pipeline *p = &ctx->pipelines[i];
    if (p->alive && p->parent == parent + 1 &&
        !memcmp(p->immutable, samplers, sizeof(samplers))) {
      *out = p;
      return RN_OK;
    }
  }

  struct rn_pipeline_desc_t desc = base->source;
  struct rn_vertex_buffer_layout_t bindings[RN_VK_MAX_BINDINGS];
  struct rn_vertex_attribute_t attributes[32];
  memcpy(bindings, base->source_bindings, sizeof(bindings));
  memcpy(attributes, base->source_attributes, sizeof(attributes));
  size_t offset = 0;
  for (size_t i = 0; i < desc.vertex_binding_count; ++i) {
    bindings[i].attributes = attributes + offset;
    offset += bindings[i].attribute_count;
  }
  desc.vertex_bindings = bindings;
  rn_pipeline_handle_t handle;
  rn_result_t result =
      _create_pipeline_samplers(ctx, &desc, samplers, descriptors, &handle);
  if (result != RN_OK)
    return result;
  ctx->pipelines[handle].parent = parent + 1;
  *out = &ctx->pipelines[handle];
  return RN_OK;
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

  for (size_t i = 0; i < ctx->pipeline_count; ++i)
    if (ctx->pipelines[i].alive && ctx->pipelines[i].parent == handle + 1)
      _destroy_pipeline(ctx, (rn_pipeline_handle_t)i);

  for (uint32_t i = 0; i < RN_VK_FRAMES_IN_FLIGHT; ++i) {
    struct rn_vk_descriptor_cache *cache = &pipeline->descriptors[i];
    struct rn_vk_descriptor_entry *entry = cache->entries;

    while (entry) {
      struct rn_vk_descriptor_entry *next = entry->next;
      ctx->alloc->free(entry, ctx->alloc->ctx);
      entry = next;
    }

    struct rn_vk_descriptor_page *page = cache->pages;

    while (page) {
      struct rn_vk_descriptor_page *next = page->next;
      vkDestroyDescriptorPool(ctx->device, page->pool, NULL);
      ctx->alloc->free(page, ctx->alloc->ctx);
      page = next;
    }
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
  ctx->bound_pipeline = pipeline->pipeline;
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
  if (texture->dmabuf && !texture->acquired)
    return RN_ERROR_BAD_STATE;

  ctx->bound_textures[slot] = handle;
  return RN_OK;
}

static rn_result_t _descriptor_for_draw(struct rn_vk_context *ctx,
                                        struct rn_vk_pipeline *pipeline,
                                        VkDescriptorSet *out) {
  struct rn_vk_descriptor_cache *cache =
      &pipeline->descriptors[ctx->frame_index];
  const uint32_t count = pipeline->descriptor_count;
  rn_texture_handle_t textures[RN_VK_MAX_TEXTURES];
  rn_texture_handle_t fallback = ctx->bound_textures[0];

  if (!_texture(ctx, fallback))
    return RN_ERROR_BAD_STATE;

  for (uint32_t slot = 0; slot < count; ++slot) {
    rn_texture_handle_t handle = ctx->bound_textures[slot];
    textures[slot] = _texture(ctx, handle) ? handle : fallback;
  }

  /* entries before unused belong to this frame. keep their sets unchanged
   * because earlier draws may still reference them */
  for (struct rn_vk_descriptor_entry *entry = cache->entries;
       entry != cache->unused; entry = entry->next) {

    if (entry->count == count &&
        !memcmp(textures, entry->textures, count * sizeof(*textures))) {
      /* cache hit */
      *out = entry->set;
      return RN_OK;
    }
  }

  struct rn_vk_descriptor_entry *entry = cache->unused;

  if (entry) {
    cache->unused = entry->next;
    goto update;
  }

  entry = ctx->alloc->alloc(sizeof(*entry), ctx->alloc->ctx);

  if (!entry)
    return RN_ERROR_OUT_OF_MEMORY;
  memset(entry, 0, sizeof(*entry));

  struct rn_vk_descriptor_page *page = cache->pages;

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
        .descriptorCount =
            RN_VK_DESCRIPTOR_PAGE_SIZE * pipeline->pool_descriptors,
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

    new_page->next = cache->pages;
    cache->pages = new_page;
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

  entry->next = cache->entries;
  cache->entries = entry;

update:
  entry->count = count;
  memcpy(entry->textures, textures, count * sizeof(*textures));
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
  *out = entry->set;
  return RN_OK;
}

rn_result_t _compile_shader(struct rn_vk_context *ctx, const char *glsl,
                            VkShaderStageFlagBits stage, uint32_t **spv,
                            size_t *spv_size) {

  if (!glsl || !spv || !spv_size)
    return RN_ERROR_INVALID_ARGUMENT;

  *spv = NULL;
  *spv_size = 0;

  shaderc_shader_kind kind;

  switch (stage) {
  case VK_SHADER_STAGE_VERTEX_BIT:
    kind = shaderc_vertex_shader;
    break;
  case VK_SHADER_STAGE_FRAGMENT_BIT:
    kind = shaderc_fragment_shader;
    break;
  default:
    return RN_ERROR_INVALID_ARGUMENT;
  }

  shaderc_compiler_t compiler = shaderc_compiler_initialize();

  if (!compiler)
    return RN_ERROR_OUT_OF_MEMORY;

  shaderc_compile_options_t options = shaderc_compile_options_initialize();

  if (!options) {
    shaderc_compiler_release(compiler);
    return RN_ERROR_OUT_OF_MEMORY;
  }

  const char *slots = ctx->texture_slots == 4   ? "4"
                      : ctx->texture_slots == 8 ? "8"
                                                : "16";
  shaderc_compile_options_add_macro_definition(options, "RN_TEXTURE_SLOTS",
                                               strlen("RN_TEXTURE_SLOTS"),
                                               slots, strlen(slots));

  shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                         shaderc_env_version_vulkan_1_0);

  shaderc_compilation_result_t result = shaderc_compile_into_spv(
      compiler, glsl, strlen(glsl), kind,
      stage == VK_SHADER_STAGE_VERTEX_BIT ? "shader.vert" : "shader.frag",
      "main", options);

  rn_result_t status = RN_ERROR_BACKEND;

  if (!result) {
    status = RN_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }

  if (shaderc_result_get_compilation_status(result) !=
      shaderc_compilation_status_success) {
    fprintf(stderr, "Shader compilation error:\n%s\n",
            shaderc_result_get_error_message(result));
    goto cleanup;
  }

  size_t size = shaderc_result_get_length(result);
  uint32_t *data = malloc(size);

  if (!data) {
    status = RN_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }

  memcpy(data, shaderc_result_get_bytes(result), size);

  *spv = data;
  *spv_size = size;
  status = RN_OK;

cleanup:

  if (result)
    shaderc_result_release(result);

  shaderc_compile_options_release(options);
  shaderc_compiler_release(compiler);

  return status;
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

  VkDescriptorSet set = VK_NULL_HANDLE;
  rn_result_t result = _sampling_pipeline(ctx, &pipeline);
  if (result != RN_OK)
    return result;
  if (ctx->bound_pipeline != pipeline->pipeline) {
    vkCmdBindPipeline(ctx->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline->pipeline);
    ctx->bound_pipeline = pipeline->pipeline;
  }
  result = _descriptor_for_draw(ctx, pipeline, &set);

  if (result != RN_OK)
    return result;

  if (pipeline->shader == RN_PIPELINE_UI_QUAD_DEDICATED) {
    uint32_t metadata[RN_VK_MAX_TEXTURES] = {0};
    struct rn_vk_texture *fallback = _texture(ctx, ctx->bound_textures[0]);
    for (uint32_t i = 0; i < pipeline->descriptor_count; ++i) {
      struct rn_vk_texture *t = _texture(ctx, ctx->bound_textures[i]);
      metadata[i] = (t ? t : fallback)->packed_yuv;
    }
    vkCmdPushConstants(ctx->command_buffer, pipeline->layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 8,
                       pipeline->descriptor_count * sizeof(uint32_t), metadata);
  }

  /* bind descriptor set */
  vkCmdBindDescriptorSets(ctx->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline->layout, 0, 1, &set, 0, NULL);

  /* draw call */
  vkCmdDrawIndexed(ctx->command_buffer, draw->index_count, draw->instance_count,
                   draw->index_offset, draw->vertex_offset,
                   draw->instance_offset);
  return RN_OK;
}

/* Uploads are synchronous, use the private command pool and
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

  /* uploads finish before returning, so the mapped staging buffer can be
   * reused for the next image and only needs to grow for larger uploads */
  if (transfer_size > ctx->staging_capacity) {
    if (ctx->staging)
      vmaDestroyBuffer(ctx->vma, ctx->staging, ctx->staging_allocation);

    ctx->staging = VK_NULL_HANDLE;
    ctx->staging_allocation = NULL;
    ctx->staging_map = NULL;
    ctx->staging_capacity = 0;
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = transfer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation_info = {
        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VmaAllocationInfo allocation;
    VkResult status =
        vmaCreateBuffer(ctx->vma, &buffer_info, &allocation_info, &ctx->staging,
                        &ctx->staging_allocation, &allocation);

    if (status != VK_SUCCESS)
      return _result(ctx, status, "vmaCreateBuffer(texture staging)");

    ctx->staging_map = allocation.pMappedData;
    ctx->staging_capacity = transfer_size;
  }

  memcpy(ctx->staging_map, pixels, transfer_size);
  VkResult status =
      vmaFlushAllocation(ctx->vma, ctx->staging_allocation, 0, transfer_size);

  if (status != VK_SUCCESS)
    return _result(ctx, status, "vmaFlushAllocation(texture staging)");

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

    vkCmdCopyBufferToImage(ctx->upload_command, ctx->staging, texture->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    _image_barrier(ctx, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture->layers);
    result = _end_upload(ctx);
  }

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

  rn_texture_handle_t handle = _texture_slot(ctx);

  if (handle == RN_INVALID_HANDLE)
    return RN_ERROR_OUT_OF_MEMORY;

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
  *out = handle;
  ctx->textures[handle] = texture;
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

static rn_result_t _write_texture(void *backend_ctx,
                                  const struct rn_texture_array_write_t *write,
                                  const void *pixels, size_t size) {
  struct rn_vk_context *ctx = backend_ctx;

  if (!ctx || !write)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_vk_texture *texture = _texture(ctx, write->texture);

  if (!texture || texture->borrowed || texture->array || write->layer)
    return RN_ERROR_INVALID_ARGUMENT;

  return _upload_image(ctx, texture, 0, write->x, write->y, write->width,
                       write->height, pixels, size);
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

  if (texture->dmabuf && texture->acquired)
    _dmabuf_release_texture(ctx, texture);

  _dmabuf_forget(ctx, handle);

  if (!ctx->tearing_down)
    vkDeviceWaitIdle(ctx->device);

  if (!texture->conversion)
    vkDestroySampler(ctx->device, texture->sampler, NULL);
  vkDestroyImageView(ctx->device, texture->view, NULL);

  if (texture->dmabuf) {
    vkDestroyImage(ctx->device, texture->image, NULL);
    for (uint32_t i = 0; i < texture->dmabuf_memory_count; ++i)
      vkFreeMemory(ctx->device, texture->dmabuf_memory[i], NULL);
    _dmabuf_close(texture);
  } else if (!texture->borrowed) {
    vmaDestroyImage(ctx->vma, texture->image, texture->allocation);
  }
  texture->alive = false;
  _VK_TRACE(ctx->log, "destroyed texture %u", (unsigned)handle);
}

bool rn_vk_backend_implement(struct rn_backend_impl_t *impl,
                             const struct rn_allocator_t *alloc,
                             const struct rn_log_t *log,
                             const void *backend_info,
                             const char *frag_dedicated,
                             const char *frag_atlas) {
  const struct rn_vk_init_options_t *options = backend_info;

  if (!impl || !alloc || !alloc->alloc || !alloc->realloc || !alloc->free ||
      !options ||
      (options->mode != RN_VK_TARGET_MANAGED &&
       options->mode != RN_VK_TARGET_EXTERNAL))
    return false;

  struct rn_vk_context *ctx = alloc->alloc(sizeof(*ctx), alloc->ctx);

  if (!ctx)
    return false;

  memset(ctx, 0, sizeof(*ctx));

  ctx->texture_slots = RN_VK_MAX_TEXTURES;
  ctx->frag_dedicated = frag_dedicated;
  ctx->frag_atlas = frag_atlas;

  ctx->alloc = alloc;
  ctx->log = log;
  ctx->config = *options;
  ctx->mode = options->mode;
  ctx->active_target = RN_VK_INVALID_TARGET;
  ctx->pipeline = RN_INVALID_HANDLE;
  ctx->index_buffer = RN_INVALID_HANDLE;

  if (_create_instance(ctx) != RN_OK)
    goto failed;

  if (_create_device(ctx) != RN_OK)
    goto failed;

  ctx->swap_format = options->external_format;

  if (ctx->mode == RN_VK_TARGET_MANAGED ||
      ctx->swap_format == VK_FORMAT_UNDEFINED)
    ctx->swap_format = VK_FORMAT_B8G8R8A8_UNORM;

  if (_create_render_pass(ctx, ctx->swap_format) != RN_OK)
    goto failed;

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

  if (_dmabuf_init(ctx) != RN_OK)
    goto failed;

  *impl = _rn_vk_dispatch;
  impl->ctx = ctx;
  impl->texture_slots = ctx->texture_slots;

  _VK_TRACE(log, "Vulkan 1.0 backend ready (GPU=%s, mode=%s)",
            ctx->properties.deviceName,
            ctx->mode == RN_VK_TARGET_MANAGED ? "managed" : "external");

  return true;

failed:

  if (ctx->device)
    vkDeviceWaitIdle(ctx->device);

  if (ctx->device)
    vkDestroySampler(ctx->device, ctx->immutable_rgb, NULL);

  if (ctx->device && ctx->upload_fence)
    vkDestroyFence(ctx->device, ctx->upload_fence, NULL);

  if (ctx->device && ctx->upload_pool)
    vkDestroyCommandPool(ctx->device, ctx->upload_pool, NULL);

  if (ctx->staging)
    vmaDestroyBuffer(ctx->vma, ctx->staging, ctx->staging_allocation);

  if (ctx->vma)
    vmaDestroyAllocator(ctx->vma);

  if (ctx->device)
    _destroy_frame_commands(ctx);

  if (ctx->device) {
    _destroy_external_targets(ctx);

    if (ctx->render_pass)
      vkDestroyRenderPass(ctx->device, ctx->render_pass, NULL);

    vkDestroyDevice(ctx->device, NULL);
  }

  if (ctx->instance)
    vkDestroyInstance(ctx->instance, NULL);

  alloc->free(ctx->dmabuf_formats, alloc->ctx);
  alloc->free(ctx->acquired_images, alloc->ctx);
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

  vkDestroySampler(ctx->device, ctx->immutable_rgb, NULL);
  while (ctx->conversions) {
    struct rn_vk_conversion *c = ctx->conversions;
    ctx->conversions = c->next;
    vkDestroySampler(ctx->device, c->sampler, NULL);
    ctx->destroy_conversion(ctx->device, c->conversion, NULL);
    alloc->free(c, alloc->ctx);
  }

  if (ctx->upload_fence)
    vkDestroyFence(ctx->device, ctx->upload_fence, NULL);

  if (ctx->upload_pool)
    vkDestroyCommandPool(ctx->device, ctx->upload_pool, NULL);

  if (ctx->staging)
    vmaDestroyBuffer(ctx->vma, ctx->staging, ctx->staging_allocation);

  if (ctx->vma)
    vmaDestroyAllocator(ctx->vma);

  _destroy_frame_commands(ctx);

  while (ctx->surfaces) {
    ctx->current_surface = ctx->surfaces;
    ctx->surfaces = ctx->surfaces->next;
    _destroy_swapchain(ctx);
    vkDestroySurfaceKHR(ctx->instance, ctx->current_surface->vk_surface, NULL);
    alloc->free(ctx->current_surface, alloc->ctx);
  }

  _destroy_external_targets(ctx);

  if (ctx->render_pass)
    vkDestroyRenderPass(ctx->device, ctx->render_pass, NULL);
  vkDestroyDevice(ctx->device, NULL);
  vkDestroyInstance(ctx->instance, NULL);

  alloc->free(ctx->dmabuf_formats, alloc->ctx);
  alloc->free(ctx->acquired_images, alloc->ctx);
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
rn_vk_surface_create(struct rn_state_t *state,
                     const struct rn_vk_surface_impl_t *impl,
                     struct rn_vk_managed_surface_t **out_managed_surface) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || ctx->mode != RN_VK_TARGET_MANAGED || ctx->frame_active || !impl ||
      !impl->create_vk_surface || !impl->framebuffer_size ||
      !out_managed_surface)
    return RN_ERROR_INVALID_ARGUMENT;

  *out_managed_surface = NULL;
  struct rn_vk_managed_surface_t *surface =
      ctx->alloc->alloc(sizeof(*surface), ctx->alloc->ctx);

  if (!surface)
    return RN_ERROR_OUT_OF_MEMORY;

  memset(surface, 0, sizeof(*surface));
  surface->impl = *impl;
  rn_result_t result = _result(
      ctx,
      impl->create_vk_surface(impl->user, ctx->instance, &surface->vk_surface),
      "create_vk_surface");

  if (result == RN_OK && !surface->vk_surface)
    result = RN_ERROR_BACKEND;

  surface->present_family = UINT32_MAX;

  for (uint32_t i = 0; result == RN_OK && i < ctx->queue_family_count; ++i) {
    VkBool32 supported = VK_FALSE;
    result =
        _result(ctx,
                vkGetPhysicalDeviceSurfaceSupportKHR(
                    ctx->physical_device, i, surface->vk_surface, &supported),
                "vkGetPhysicalDeviceSurfaceSupportKHR");

    if (supported) {
      surface->present_family = i;

      if (i == ctx->queue_family)
        break;
    }
  }

  if (result == RN_OK && surface->present_family == UINT32_MAX)
    result = RN_ERROR_BACKEND;

  struct rn_vk_managed_surface_t *previous = ctx->current_surface;
  ctx->current_surface = surface;

  if (result == RN_OK) {
    vkGetDeviceQueue(ctx->device, surface->present_family, 0,
                     &surface->present_queue);
    uint32_t width = 0, height = 0;
    impl->framebuffer_size(impl->user, &width, &height);

    if (width && height)
      result = _create_swapchain(ctx, width, height);
  }

  if (result != RN_OK) {
    _destroy_swapchain(ctx);

    if (surface->vk_surface)
      vkDestroySurfaceKHR(ctx->instance, surface->vk_surface, NULL);
    ctx->current_surface = previous;
    ctx->alloc->free(surface, ctx->alloc->ctx);
    return result;
  }

  surface->next = ctx->surfaces;
  ctx->surfaces = surface;
  *out_managed_surface = surface;
  return RN_OK;
}

void rn_vk_surface_set_current(struct rn_state_t *state,
                               struct rn_vk_managed_surface_t *surface) {
  ((struct rn_vk_context *)state->backend.ctx)->current_surface = surface;
}

rn_result_t rn_vk_surface_destroy(struct rn_state_t *state,
                                  struct rn_vk_managed_surface_t *surface) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || ctx->frame_active || !surface)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_vk_managed_surface_t **link = &ctx->surfaces;

  while (*link && *link != surface)
    link = &(*link)->next;

  if (!*link)
    return RN_ERROR_INVALID_ARGUMENT;

  rn_result_t result =
      _result(ctx, vkDeviceWaitIdle(ctx->device), "vkDeviceWaitIdle(surface)");

  if (result != RN_OK)
    return result;

  struct rn_vk_managed_surface_t *previous = ctx->current_surface;
  ctx->current_surface = surface;
  _destroy_swapchain(ctx);
  vkDestroySurfaceKHR(ctx->instance, surface->vk_surface, NULL);
  ctx->current_surface = previous == surface ? NULL : previous;
  *link = surface->next;
  ctx->alloc->free(surface, ctx->alloc->ctx);
  return RN_OK;
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

  if (!ctx || ctx->frame_active || ctx->fatal)
    return RN_ERROR_BAD_STATE;

  if (!ctx->frame_submitted)
    return _dmabuf_release_pending(ctx);

  uint32_t last =
      (ctx->frame_index + RN_VK_FRAMES_IN_FLIGHT - 1) % RN_VK_FRAMES_IN_FLIGHT;

  rn_result_t result =
      _result(ctx,
              vkWaitForFences(ctx->device, 1, &ctx->frames[last].fence, VK_TRUE,
                              UINT64_MAX),
              "vkWaitForFences(render complete)");

  if (result != RN_OK)
    return result;

  ctx->completed = ctx->submitted;
  return _dmabuf_release_pending(ctx);
}

rn_result_t rn_vk_submission_status(struct rn_state_t *state,
                                    uint64_t *o_last_submitted,
                                    uint64_t *o_last_completed) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || ctx->frame_active || !o_last_submitted || !o_last_completed)
    return RN_ERROR_INVALID_ARGUMENT;

  for (uint32_t i = 0; i < RN_VK_FRAMES_IN_FLIGHT; ++i) {
    struct rn_vk_frame *frame = &ctx->frames[i];
    /* skip already completed submissions */
    if (frame->submission <= ctx->completed)
      continue;

    VkResult status = vkGetFenceStatus(ctx->device, frame->fence);
    if (status == VK_SUCCESS)
      ctx->completed = frame->submission;
    else if (status != VK_NOT_READY)
      /* the frame must be ready at this point */
      return _result(ctx, status, "vkGetFenceStatus(frame)");
  }

  *o_last_submitted = ctx->submitted;
  *o_last_completed = ctx->completed;

  return RN_OK;
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

static rn_result_t _wrap_vk_image(struct rn_vk_context *ctx, VkImage image,
                                  VkFormat format, uint32_t width,
                                  uint32_t height, bool opaque,
                                  const VkComponentMapping *components,
                                  struct rn_vk_conversion *conversion,
                                  struct rn_image_t *out_image) {

  if (!ctx || !out_image || !image || !width || !height || ctx->frame_active)
    return RN_ERROR_INVALID_ARGUMENT;

  rn_texture_handle_t handle = _texture_slot(ctx);

  if (handle == RN_INVALID_HANDLE)
    return RN_ERROR_OUT_OF_MEMORY;

  struct rn_vk_texture texture = {0};

  texture.image = image;
  texture.width = width;
  texture.height = height;
  texture.layers = 1;
  texture.borrowed = true;

  VkSamplerYcbcrConversionInfo conversion_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
      .conversion = conversion ? conversion->conversion : VK_NULL_HANDLE,
  };
  VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = conversion ? &conversion_info : NULL,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .components = {.a = opaque ? VK_COMPONENT_SWIZZLE_ONE
                                 : VK_COMPONENT_SWIZZLE_IDENTITY},
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .levelCount = 1,
              .layerCount = 1,
          },
  };

  if (components)
    view_info.components = *components;
  if (opaque)
    view_info.components.a = VK_COMPONENT_SWIZZLE_ONE;
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

  texture.conversion = conversion;
  if (conversion)
    texture.sampler = conversion->sampler;
  else
    status =
        vkCreateSampler(ctx->device, &sampler_info, NULL, &texture.sampler);

  if (status != VK_SUCCESS) {
    vkDestroyImageView(ctx->device, texture.view, NULL);
    return _result(ctx, status, "vkCreateSampler(client texture)");
  }

  texture.alive = true;
  ctx->textures[handle] = texture;

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

rn_result_t rn_image_from_vk_image(struct rn_state_t *state, VkImage image,
                                   VkFormat format, uint32_t width,
                                   uint32_t height,
                                   struct rn_image_t *out_image) {
  return _wrap_vk_image(_state_context(state), image, format, width, height,
                        false, NULL, NULL, out_image);
}

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define RN_FOURCC(a, b, c, d)                                                  \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |              \
   ((uint32_t)(d) << 24))

/* DRM fourcc describes pixels; modifiers describe layout and are always
 * enumerated by the driver, never decoded here. X formats force alpha to one.
 */
struct rn_vk_drm_format {
  uint32_t drm;
  VkFormat vk;
  bool opaque, yuv, swap_rb, two_plane_444, packed;
  uint8_t subsampled;
  VkComponentMapping components;
};

#define RGB(a, b, c, d, fmt)                                                   \
  {.drm = RN_FOURCC(a, b, c, d), .vk = VK_FORMAT_##fmt}
#define OPAQUE(a, b, c, d, fmt)                                                \
  {.drm = RN_FOURCC(a, b, c, d), .vk = VK_FORMAT_##fmt, .opaque = true}
#define YUV_LAYOUT(a, b, c, d, fmt, swap, sub)                                 \
  {.drm = RN_FOURCC(a, b, c, d),                                               \
   .vk = VK_FORMAT_##fmt,                                                      \
   .yuv = true,                                                                \
   .swap_rb = swap,                                                            \
   .subsampled = sub}
#define YUV(a, b, c, d, fmt, swap) YUV_LAYOUT(a, b, c, d, fmt, swap, 0)
#define YUV420(a, b, c, d, fmt, swap) YUV_LAYOUT(a, b, c, d, fmt, swap, 3)
#define YUV422(a, b, c, d, fmt, swap) YUV_LAYOUT(a, b, c, d, fmt, swap, 1)
#define SWIZZLE(a, b, c, d, fmt, r, g, bl, al)                                 \
  {                                                                            \
    .drm = RN_FOURCC(a, b, c, d), .vk = VK_FORMAT_##fmt, .components = {       \
      VK_COMPONENT_SWIZZLE_##r,                                                \
      VK_COMPONENT_SWIZZLE_##g,                                                \
      VK_COMPONENT_SWIZZLE_##bl,                                               \
      VK_COMPONENT_SWIZZLE_##al                                                \
    }                                                                          \
  }
#define YUV444(a, b, c, d, fmt, swap)                                          \
  {.drm = RN_FOURCC(a, b, c, d),                                               \
   .vk = VK_FORMAT_##fmt,                                                      \
   .yuv = true,                                                                \
   .swap_rb = swap,                                                            \
   .two_plane_444 = true}
#define YUV_SWIZZLE(a, b, c, d, fmt, r, g, bl, al)                             \
  {                                                                            \
    .drm = RN_FOURCC(a, b, c, d), .vk = VK_FORMAT_##fmt, .yuv = true,          \
    .packed = true, .components = {                                            \
      VK_COMPONENT_SWIZZLE_##r,                                                \
      VK_COMPONENT_SWIZZLE_##g,                                                \
      VK_COMPONENT_SWIZZLE_##bl,                                               \
      VK_COMPONENT_SWIZZLE_##al                                                \
    }                                                                          \
  }

// clang-format off
static const struct rn_vk_drm_format _dmabuf_formats[] = {
    RGB('A','R','2','4', B8G8R8A8_UNORM),
    OPAQUE('X','R','2','4', B8G8R8A8_UNORM),
    RGB('A','B','2','4', R8G8B8A8_UNORM),
    OPAQUE('X','B','2','4', R8G8B8A8_UNORM),
    RGB('R','G','2','4', B8G8R8_UNORM),
    RGB('B','G','2','4', R8G8B8_UNORM),
    RGB('R','G','1','6', R5G6B5_UNORM_PACK16),
    RGB('B','G','1','6', B5G6R5_UNORM_PACK16),
    RGB('A','R','1','5', A1R5G5B5_UNORM_PACK16),
    OPAQUE('X','R','1','5', A1R5G5B5_UNORM_PACK16),
    RGB('R','A','1','5', R5G5B5A1_UNORM_PACK16),
    OPAQUE('R','X','1','5', R5G5B5A1_UNORM_PACK16),
    RGB('B','A','1','5', B5G5R5A1_UNORM_PACK16),
    OPAQUE('B','X','1','5', B5G5R5A1_UNORM_PACK16),
    RGB('R','A','1','2', R4G4B4A4_UNORM_PACK16),
    OPAQUE('R','X','1','2', R4G4B4A4_UNORM_PACK16),
    RGB('B','A','1','2', B4G4R4A4_UNORM_PACK16),
    OPAQUE('B','X','1','2', B4G4R4A4_UNORM_PACK16),
    RGB('A','R','3','0', A2R10G10B10_UNORM_PACK32),
    OPAQUE('X','R','3','0', A2R10G10B10_UNORM_PACK32),
    RGB('A','B','3','0', A2B10G10R10_UNORM_PACK32),
    OPAQUE('X','B','3','0', A2B10G10R10_UNORM_PACK32),
    RGB('A','B','4','8', R16G16B16A16_UNORM),
    OPAQUE('X','B','4','8', R16G16B16A16_UNORM),
    RGB('A','B','4','H', R16G16B16A16_SFLOAT),
    OPAQUE('X','B','4','H', R16G16B16A16_SFLOAT),
    SWIZZLE('R','A','2','4', R8G8B8A8_UNORM, A,B,G,R),
    SWIZZLE('R','X','2','4', R8G8B8A8_UNORM, A,B,G,ONE),
    SWIZZLE('B','A','2','4', R8G8B8A8_UNORM, G,B,A,R),
    SWIZZLE('B','X','2','4', R8G8B8A8_UNORM, G,B,A,ONE),
    SWIZZLE('A','B','1','5', A1R5G5B5_UNORM_PACK16, B,G,R,A),
    SWIZZLE('X','B','1','5', A1R5G5B5_UNORM_PACK16, B,G,R,ONE),
    SWIZZLE('A','R','1','2', R4G4B4A4_UNORM_PACK16, G,B,A,R),
    SWIZZLE('X','R','1','2', R4G4B4A4_UNORM_PACK16, G,B,A,ONE),
    SWIZZLE('A','B','1','2', R4G4B4A4_UNORM_PACK16, A,B,G,R),
    SWIZZLE('X','B','1','2', R4G4B4A4_UNORM_PACK16, A,B,G,ONE),
    SWIZZLE('A','R','4','8', R16G16B16A16_UNORM, B,G,R,A),
    SWIZZLE('X','R','4','8', R16G16B16A16_UNORM, B,G,R,ONE),
    SWIZZLE('A','R','4','H', R16G16B16A16_SFLOAT, B,G,R,A),
    SWIZZLE('X','R','4','H', R16G16B16A16_SFLOAT, B,G,R,ONE),
    RGB('B','G','4','8', R16G16B16_UNORM),
    SWIZZLE('R','G','4','8', R16G16B16_UNORM, B,G,R,ONE),
    RGB('G','R',' ','H', R16G16_SFLOAT),
    RGB('B','G','R','H', R16G16B16_SFLOAT),
    RGB('R','8',' ',' ', R8_UNORM),
    RGB('R','1','6',' ', R16_UNORM),
    RGB('G','R','8','8', R8G8_UNORM),
    RGB('G','R','3','2', R16G16_UNORM),
    SWIZZLE('R','G','8','8', R8G8_UNORM, G,R,ZERO,ONE),
    SWIZZLE('R','G','3','2', R16G16_UNORM, G,R,ZERO,ONE),
    YUV420('N','V','1','2', G8_B8R8_2PLANE_420_UNORM, false),
    YUV420('N','V','2','1', G8_B8R8_2PLANE_420_UNORM, true),
    YUV422('N','V','1','6', G8_B8R8_2PLANE_422_UNORM, false),
    YUV422('N','V','6','1', G8_B8R8_2PLANE_422_UNORM, true),
    YUV420('Y','U','1','2', G8_B8_R8_3PLANE_420_UNORM, false),
    YUV420('Y','V','1','2', G8_B8_R8_3PLANE_420_UNORM, true),
    YUV422('Y','U','1','6', G8_B8_R8_3PLANE_422_UNORM, false),
    YUV422('Y','V','1','6', G8_B8_R8_3PLANE_422_UNORM, true),
    YUV('Y','U','2','4', G8_B8_R8_3PLANE_444_UNORM, false),
    YUV('Y','V','2','4', G8_B8_R8_3PLANE_444_UNORM, true),
    YUV422('Y','U','Y','V', G8B8G8R8_422_UNORM, false),
    YUV422('Y','V','Y','U', G8B8G8R8_422_UNORM, true),
    YUV422('U','Y','V','Y', B8G8R8G8_422_UNORM, false),
    YUV422('V','Y','U','Y', B8G8R8G8_422_UNORM, true),
    YUV444('N','V','2','4', G8_B8R8_2PLANE_444_UNORM_EXT, false),
    YUV444('N','V','4','2', G8_B8R8_2PLANE_444_UNORM_EXT, true),
    YUV444('P','4','1','0', G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16_EXT, false),
    YUV444('P','4','1','2', G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16_EXT, false),
    YUV444('P','4','1','6', G16_B16R16_2PLANE_444_UNORM_EXT, false),
    YUV_SWIZZLE('A','Y','U','V', R8G8B8A8_UNORM, R,B,G,A),
    YUV_SWIZZLE('X','Y','U','V', R8G8B8A8_UNORM, R,B,G,ONE),
    YUV_SWIZZLE('A','V','U','Y', R8G8B8A8_UNORM, B,R,G,A),
    YUV_SWIZZLE('X','V','U','Y', R8G8B8A8_UNORM, B,R,G,ONE),
    YUV_SWIZZLE('V','U','2','4', R8G8B8_UNORM, B,R,G,ONE),
    YUV_SWIZZLE('X','Y','3','0', A2B10G10R10_UNORM_PACK32, B,R,G,ONE),
    YUV_SWIZZLE('Y','4','1','0', A2B10G10R10_UNORM_PACK32, B,G,R,A),
    YUV_SWIZZLE('Y','4','1','2', R16G16B16A16_UNORM, B,G,R,A),
    YUV_SWIZZLE('Y','4','1','6', R16G16B16A16_UNORM, B,G,R,A),
    YUV422('Y','2','1','0', G10X6B10X6G10X6R10X6_422_UNORM_4PACK16, false),
    YUV422('Y','2','1','2', G12X4B12X4G12X4R12X4_422_UNORM_4PACK16, false),
    YUV422('Y','2','1','6', G16B16G16R16_422_UNORM, false),
    YUV420('P','0','1','0', G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, false),
    YUV420('P','0','1','2', G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16, false),
    YUV420('P','0','1','6', G16_B16R16_2PLANE_420_UNORM, false),
    YUV422('P','2','1','0', G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16, false),
    YUV422('P','2','1','2', G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16, false),
    YUV422('P','2','1','6', G16_B16R16_2PLANE_422_UNORM, false),
};
// clang-format on
#undef RGB
#undef OPAQUE
#undef YUV
#undef YUV_LAYOUT
#undef YUV420
#undef YUV422
#undef SWIZZLE
#undef YUV444
#undef YUV_SWIZZLE

static bool _dmabuf_importable(struct rn_vk_context *ctx, VkFormat format,
                               uint64_t modifier, VkImageCreateFlags flags,
                               uint32_t *descriptors,
                               const struct rn_vk_dmabuf_desc_t *desc) {
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
      .drmFormatModifier = modifier,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkPhysicalDeviceExternalImageFormatInfo external = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .pNext = &mod,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  VkPhysicalDeviceImageFormatInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &external,
      .format = format,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
      .flags = flags,
  };
  VkSamplerYcbcrConversionImageFormatProperties ycbcr = {
      .sType =
          VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES,
  };
  VkExternalImageFormatProperties external_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
      .pNext = descriptors ? &ycbcr : NULL,
  };
  VkImageFormatProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      .pNext = &external_props,
  };

  VkResult status = ctx->image_properties(ctx->physical_device, &info, &props);
  if (descriptors)
    *descriptors = ycbcr.combinedImageSamplerDescriptorCount;
  if (status == VK_SUCCESS && desc &&
      (desc->width > props.imageFormatProperties.maxExtent.width ||
       desc->height > props.imageFormatProperties.maxExtent.height))
    return false;
  if (flags & VK_IMAGE_CREATE_DISJOINT_BIT &&
      external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
    return false;
  return status == VK_SUCCESS &&
         (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

static rn_result_t _dmabuf_conversion(struct rn_vk_context *ctx,
                                      const struct rn_vk_drm_format *format,
                                      const struct rn_vk_dmabuf_desc_t *desc,
                                      struct rn_vk_conversion **out) {
  *out = NULL;
  if (!format->yuv || format->packed)
    return RN_OK;
  if ((unsigned)desc->ycbcr_model > RN_VK_YCBCR_BT2020 ||
      (unsigned)desc->ycbcr_range > RN_VK_YCBCR_FULL ||
      (unsigned)desc->chroma_x > RN_VK_CHROMA_MIDPOINT ||
      (unsigned)desc->chroma_y > RN_VK_CHROMA_MIDPOINT)
    return RN_ERROR_INVALID_ARGUMENT;

  VkDrmFormatModifierPropertiesListEXT list = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                               .pNext = &list};

  ctx->format_properties(ctx->physical_device, format->vk, &props);
  VkDrmFormatModifierPropertiesEXT *mods = ctx->alloc->alloc(
      list.drmFormatModifierCount * sizeof(*mods), ctx->alloc->ctx);
  if (!mods)
    return RN_ERROR_OUT_OF_MEMORY;

  list.pDrmFormatModifierProperties = mods;
  ctx->format_properties(ctx->physical_device, format->vk, &props);
  VkFormatFeatureFlags flags = 0;

  for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i)
    if (mods[i].drmFormatModifier == desc->modifier)
      flags = mods[i].drmFormatModifierTilingFeatures;

  ctx->alloc->free(mods, ctx->alloc->ctx);

  VkChromaLocation locations[2];

  unsigned requested[2] = {desc->chroma_x, desc->chroma_y};
  for (unsigned i = 0; i < 2; ++i) {
    if (!requested[i])
      requested[i] = flags & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT
                         ? RN_VK_CHROMA_COSITED
                         : RN_VK_CHROMA_MIDPOINT;
    VkFormatFeatureFlags required =
        requested[i] == RN_VK_CHROMA_COSITED
            ? VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT
            : VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
    if (!(flags & required))
      return RN_ERROR_INVALID_ARGUMENT;
    locations[i] = requested[i] == RN_VK_CHROMA_COSITED
                       ? VK_CHROMA_LOCATION_COSITED_EVEN
                       : VK_CHROMA_LOCATION_MIDPOINT;
  }
  static const VkSamplerYcbcrModelConversion models[] = {
      VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
      VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
      VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020};
  VkSamplerYcbcrConversionCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
      .format = format->vk,
      .ycbcrModel = models[desc->ycbcr_model],
      .ycbcrRange = desc->ycbcr_range == RN_VK_YCBCR_FULL
                        ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
                        : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
      .components = {.r = format->swap_rb ? VK_COMPONENT_SWIZZLE_B
                                          : VK_COMPONENT_SWIZZLE_IDENTITY,
                     .b = format->swap_rb ? VK_COMPONENT_SWIZZLE_R
                                          : VK_COMPONENT_SWIZZLE_IDENTITY},
      .xChromaOffset = locations[0],
      .yChromaOffset = locations[1],
      .chromaFilter =
          flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT
              ? VK_FILTER_LINEAR
              : VK_FILTER_NEAREST,
  };
  if (format->components.r || format->components.g || format->components.b ||
      format->components.a)
    info.components = format->components;
  uint32_t descriptors;
  if (!_dmabuf_importable(ctx, format->vk, desc->modifier, 0, &descriptors,
                          NULL) ||
      !descriptors)
    return RN_ERROR_INVALID_ARGUMENT;
  for (struct rn_vk_conversion *c = ctx->conversions; c; c = c->next)
    if (c->info.format == info.format &&
        c->info.ycbcrModel == info.ycbcrModel &&
        c->info.ycbcrRange == info.ycbcrRange &&
        c->info.components.r == info.components.r &&
        c->info.components.g == info.components.g &&
        c->info.components.b == info.components.b &&
        c->info.components.a == info.components.a &&
        c->info.xChromaOffset == info.xChromaOffset &&
        c->info.yChromaOffset == info.yChromaOffset &&
        c->info.chromaFilter == info.chromaFilter && c->features == flags &&
        c->descriptors == descriptors) {
      *out = c;
      return RN_OK;
    }
  struct rn_vk_conversion *c = ctx->alloc->alloc(sizeof(*c), ctx->alloc->ctx);
  if (!c)
    return RN_ERROR_OUT_OF_MEMORY;
  memset(c, 0, sizeof(*c));
  c->info = info;
  c->descriptors = descriptors;
  c->features = flags;
  rn_result_t result = _result(
      ctx, ctx->create_conversion(ctx->device, &info, NULL, &c->conversion),
      "vkCreateSamplerYcbcrConversionKHR");
  if (result != RN_OK) {
    ctx->alloc->free(c, ctx->alloc->ctx);
    return result;
  }
  VkSamplerYcbcrConversionInfo conversion = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
      .conversion = c->conversion};

  VkFilter filter = flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                        ? VK_FILTER_LINEAR
                        : VK_FILTER_NEAREST;
  if (!(flags &
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_SEPARATE_RECONSTRUCTION_FILTER_BIT))
    filter = info.chromaFilter;
  VkSamplerCreateInfo sampler = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .pNext = &conversion,
      .magFilter = filter,
      .minFilter = filter,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxAnisotropy = 1.0f,
  };
  result =
      _result(ctx, vkCreateSampler(ctx->device, &sampler, NULL, &c->sampler),
              "vkCreateSampler(YCbCr)");
  if (result != RN_OK) {
    ctx->destroy_conversion(ctx->device, c->conversion, NULL);
    ctx->alloc->free(c, ctx->alloc->ctx);
    return result;
  }
  c->next = ctx->conversions;
  ctx->conversions = c;
  *out = c;
  return RN_OK;
}

static rn_result_t _dmabuf_init(struct rn_vk_context *ctx) {
  if (!ctx->dmabuf_enabled)
    return RN_OK;

  ctx->create_conversion =
      (PFN_vkCreateSamplerYcbcrConversionKHR)vkGetDeviceProcAddr(
          ctx->device, "vkCreateSamplerYcbcrConversionKHR");
  ctx->destroy_conversion =
      (PFN_vkDestroySamplerYcbcrConversionKHR)vkGetDeviceProcAddr(
          ctx->device, "vkDestroySamplerYcbcrConversionKHR");
  if (ctx->ycbcr_enabled &&
      (!ctx->create_conversion || !ctx->destroy_conversion))
    return RN_ERROR_BACKEND;
  const VkSamplerCreateInfo rgb_sampler = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxAnisotropy = 1.0f,
  };
  rn_result_t sampler_result = _result(
      ctx,
      vkCreateSampler(ctx->device, &rgb_sampler, NULL, &ctx->immutable_rgb),
      "vkCreateSampler(immutable RGB)");
  if (sampler_result != RN_OK)
    return sampler_result;

  ctx->format_properties =
      (PFN_vkGetPhysicalDeviceFormatProperties2KHR)vkGetInstanceProcAddr(
          ctx->instance, "vkGetPhysicalDeviceFormatProperties2KHR");
  ctx->image_properties =
      (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)vkGetInstanceProcAddr(
          ctx->instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
  ctx->fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
      ctx->device, "vkGetMemoryFdPropertiesKHR");
  ctx->import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(
      ctx->device, "vkImportSemaphoreFdKHR");
  PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR semaphore_properties =
      (PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR)
          vkGetInstanceProcAddr(
              ctx->instance,
              "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");

  if (!ctx->format_properties || !ctx->image_properties ||
      !ctx->fd_properties || !ctx->import_semaphore_fd || !semaphore_properties)
    return RN_ERROR_BACKEND;

  const VkPhysicalDeviceExternalSemaphoreInfo semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
  };
  VkExternalSemaphoreProperties semaphore_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
  };
  semaphore_properties(ctx->physical_device, &semaphore_info, &semaphore_props);
  if (!(semaphore_props.externalSemaphoreFeatures &
        VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)) {
    ctx->dmabuf_enabled = false;
    return RN_OK;
  }

  uint32_t max_descriptors = 1;
  for (size_t f = 0; f < sizeof(_dmabuf_formats) / sizeof(_dmabuf_formats[0]);
       ++f) {
    if ((_dmabuf_formats[f].yuv && !_dmabuf_formats[f].packed &&
         !ctx->ycbcr_enabled) ||
        (_dmabuf_formats[f].two_plane_444 && !ctx->ycbcr_444_enabled))
      continue;
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                 .pNext = &list};

    ctx->format_properties(ctx->physical_device, _dmabuf_formats[f].vk, &props);

    if (!list.drmFormatModifierCount)
      continue;

    VkDrmFormatModifierPropertiesEXT *mods = ctx->alloc->alloc(
        list.drmFormatModifierCount * sizeof(*mods), ctx->alloc->ctx);

    if (!mods)
      return RN_ERROR_OUT_OF_MEMORY;

    list.pDrmFormatModifierProperties = mods;
    ctx->format_properties(ctx->physical_device, _dmabuf_formats[f].vk, &props);

    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
      VkFormatFeatureFlags required =
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

      if (_dmabuf_formats[f].yuv && !_dmabuf_formats[f].packed)
        required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      VkFormatFeatureFlags chroma =
          VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT |
          VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;
      if ((_dmabuf_formats[f].yuv && !_dmabuf_formats[f].packed &&
           !(mods[i].drmFormatModifierTilingFeatures & chroma)) ||
          (mods[i].drmFormatModifierTilingFeatures & required) != required ||
          !mods[i].drmFormatModifierPlaneCount ||
          mods[i].drmFormatModifierPlaneCount > RN_VK_DMABUF_MAX_PLANES ||
          !_dmabuf_importable(ctx, _dmabuf_formats[f].vk,
                              mods[i].drmFormatModifier, 0, NULL, NULL))
        continue;

      if (_dmabuf_formats[f].yuv && !_dmabuf_formats[f].packed) {
        uint32_t descriptors = 0;
        if (!_dmabuf_importable(ctx, _dmabuf_formats[f].vk,
                                mods[i].drmFormatModifier, 0, &descriptors,
                                NULL) ||
            !descriptors)
          continue;
        if (descriptors > max_descriptors)
          max_descriptors = descriptors;
      }

      struct rn_vk_dmabuf_format_t *formats = ctx->alloc->realloc(
          ctx->dmabuf_formats,
          (ctx->dmabuf_format_count + 1) * sizeof(*formats), ctx->alloc->ctx);

      if (!formats) {
        ctx->alloc->free(mods, ctx->alloc->ctx);
        return RN_ERROR_OUT_OF_MEMORY;
      }

      ctx->dmabuf_formats = formats;

      /* insert format */
      formats[ctx->dmabuf_format_count++] = (struct rn_vk_dmabuf_format_t){
          .format = _dmabuf_formats[f].drm,
          .modifier = mods[i].drmFormatModifier,
          .plane_count = mods[i].drmFormatModifierPlaneCount,
      };
    }

    ctx->alloc->free(mods, ctx->alloc->ctx);
  }

  const VkPhysicalDeviceLimits *limits = &ctx->properties.limits;
  uint32_t limit = limits->maxPerStageDescriptorSamplers;
  if (limits->maxPerStageDescriptorSampledImages < limit)
    limit = limits->maxPerStageDescriptorSampledImages;
  if (limits->maxDescriptorSetSamplers < limit)
    limit = limits->maxDescriptorSetSamplers;
  if (limits->maxDescriptorSetSampledImages < limit)
    limit = limits->maxDescriptorSetSampledImages;
  limit /= max_descriptors;
  ctx->texture_slots = limit >= 16 ? 16 : limit >= 8 ? 8 : 4;
  return RN_OK;
}

static void _dmabuf_barrier(struct rn_vk_context *ctx,
                            struct rn_vk_texture *texture,
                            VkCommandBuffer command, bool acquire) {

  /* transfer ownership between the external producer and our graphics queue.
   * use general layout outside runara and shader read layout while sampling */
  VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = acquire ? 0 : VK_ACCESS_SHADER_READ_BIT,
      .dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT : 0,
      .oldLayout = acquire ? VK_IMAGE_LAYOUT_GENERAL
                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .newLayout = acquire ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                           : VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex =
          acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : ctx->queue_family,
      .dstQueueFamilyIndex =
          acquire ? ctx->queue_family : VK_QUEUE_FAMILY_FOREIGN_EXT,
      .image = texture->image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = 1,
                           .layerCount = 1},
  };

  vkCmdPipelineBarrier(command,
                       acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                               : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       acquire ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                               : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       0, 0, NULL, 0, NULL, 1, &barrier);
}

static void _dmabuf_release_commands(struct rn_vk_context *ctx,
                                     VkCommandBuffer command) {
  for (size_t i = 0; i < ctx->acquired_count; ++i) {
    struct rn_vk_texture *texture = _texture(ctx, ctx->acquired_images[i]);

    if (texture && texture->acquired) {
      _dmabuf_barrier(ctx, texture, command, false);
    }
  }
}

static rn_result_t _dmabuf_release_pending(struct rn_vk_context *ctx) {
  /* Acquires are queued on the CPU until the render submission. Cancel unused
   * acquires, or forget those whose release barriers were just submitted. */
  for (size_t i = 0; i < ctx->acquired_count; ++i) {
    struct rn_vk_texture *texture = _texture(ctx, ctx->acquired_images[i]);
    if (texture)
      _dmabuf_release_texture(ctx, texture);
  }
  ctx->acquired_count = 0;
  return RN_OK;
}

static rn_result_t _dmabuf_release_texture(struct rn_vk_context *ctx,
                                           struct rn_vk_texture *texture) {
  (void)ctx;
  if (texture->acquire_fd >= 0)
    close(texture->acquire_fd);
  texture->acquire_fd = -1;
  texture->acquired = false;
  return RN_OK;
}

static void _dmabuf_forget(struct rn_vk_context *ctx,
                           rn_texture_handle_t handle) {
  for (size_t i = 0; i < ctx->acquired_count; ++i) {
    if (ctx->acquired_images[i] == handle) {
      ctx->acquired_images[i] = ctx->acquired_images[--ctx->acquired_count];
      break;
    }
  }
}

static void _dmabuf_close(struct rn_vk_texture *texture) {
  for (uint32_t i = 0; i < texture->dmabuf_memory_count; ++i)
    close(texture->dmabuf_fd[i]);
}

static rn_result_t _dmabuf_acquire_commands(struct rn_vk_context *ctx,
                                            struct rn_vk_frame *frame) {
  for (size_t i = 0; i < ctx->acquired_count; ++i) {
    struct rn_vk_texture *texture = _texture(ctx, ctx->acquired_images[i]);
    if (!texture || !texture->acquired)
      return RN_ERROR_BAD_STATE;
    /* Frame-slot reuse already waited for all earlier uses of these semaphores.
     */
    if (!frame->acquire_semaphores[i]) {
      const VkSemaphoreCreateInfo info = {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      };
      VkSemaphore semaphore = VK_NULL_HANDLE;
      rn_result_t result =
          _result(ctx, vkCreateSemaphore(ctx->device, &info, NULL, &semaphore),
                  "vkCreateSemaphore(dmabuf acquire)");
      if (result != RN_OK)
        return result;
      frame->acquire_semaphores[i] = semaphore;
    }
    /* Keep the queued FD for safe retry if recording fails before submission.
     */
    int fd = fcntl(texture->acquire_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
      return RN_ERROR_BACKEND;
    const VkImportSemaphoreFdInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = frame->acquire_semaphores[i],
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = fd,
    };
    VkResult status = ctx->import_semaphore_fd(ctx->device, &info);
    if (status != VK_SUCCESS) {
      close(fd);
      return _result(ctx, status, "vkImportSemaphoreFdKHR(dmabuf)");
    }
    frame->wait_semaphores[frame->wait_count] = frame->acquire_semaphores[i];
    /* Legacy queue-family transfers must wait before the ownership acquire,
     * not merely before the fragment shader reads the image. */
    frame->wait_stages[frame->wait_count++] =
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    _dmabuf_barrier(ctx, texture, frame->command, true);
  }
  return RN_OK;
}

static rn_result_t _dmabuf_acquire(struct rn_vk_context *ctx,
                                   rn_texture_handle_t handle, int fence_fd) {
  struct rn_vk_texture *texture = _texture(ctx, handle);

  if (!texture->acquired && ctx->acquired_count == ctx->acquired_capacity) {
    size_t capacity = ctx->acquired_capacity ? ctx->acquired_capacity * 2 : 16;
    rn_texture_handle_t *handles = ctx->alloc->realloc(
        ctx->acquired_images, capacity * sizeof(*handles), ctx->alloc->ctx);
    if (!handles)
      return RN_ERROR_OUT_OF_MEMORY;
    ctx->acquired_images = handles;
    ctx->acquired_capacity = capacity;
  }

  int fd;
  if (fence_fd >= 0) {
    fd = fcntl(fence_fd, F_DUPFD_CLOEXEC, 0);
  } else {
    fd = -1;
    for (uint32_t i = 0; i < texture->dmabuf_memory_count; ++i) {
      struct dma_buf_export_sync_file sync = {.flags = DMA_BUF_SYNC_READ,
                                              .fd = -1};
      if (ioctl(texture->dmabuf_fd[i], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &sync) <
          0) {
        _VK_ERROR(ctx->log, "cannot export dma-buf acquire fence: %s",
                  strerror(errno));
        if (fd >= 0)
          close(fd);
        return RN_ERROR_BACKEND;
      }
      if (fd < 0) {
        fd = sync.fd;
      } else {
        struct sync_merge_data merge = {.name = "runara-planes",
                                        .fd2 = sync.fd};
        int status = ioctl(fd, SYNC_IOC_MERGE, &merge);
        close(sync.fd);
        close(fd);
        if (status < 0)
          return RN_ERROR_BACKEND;
        fd = merge.fence;
      }
    }
  }
  if (fd < 0)
    return RN_ERROR_BACKEND;

  if (texture->acquired) {
    /* Multiple uses of one image may carry different producer dependencies. */
    struct sync_merge_data merge = {.name = "runara-acquire", .fd2 = fd};
    int status = ioctl(texture->acquire_fd, SYNC_IOC_MERGE, &merge);
    close(fd);
    if (status < 0)
      return RN_ERROR_BACKEND;
    close(texture->acquire_fd);
    texture->acquire_fd = merge.fence;
    return RN_OK;
  }

  texture->acquire_fd = fd;
  texture->acquired = true;
  ctx->acquired_images[ctx->acquired_count++] = handle;
  return RN_OK;
}

/* creates a new rn_image_t from a dmabuf description */
static rn_result_t _dmabuf_create(struct rn_vk_context *ctx,
                                  const struct rn_vk_dmabuf_desc_t *desc,
                                  const struct stat *stat,
                                  struct rn_image_t *image) {

  /* get dmabuf format (drm -> vk) from desciption format */
  size_t f;
  for (f = 0; f < sizeof(_dmabuf_formats) / sizeof(_dmabuf_formats[0]); ++f) {
    if (_dmabuf_formats[f].drm == desc->format)
      break;
  }

  if (f == sizeof(_dmabuf_formats) / sizeof(_dmabuf_formats[0]))
    return RN_ERROR_INVALID_ARGUMENT;

  if (((_dmabuf_formats[f].subsampled & 1u) && (desc->width & 1u)) ||
      ((_dmabuf_formats[f].subsampled & 2u) && (desc->height & 1u)))
    return RN_ERROR_INVALID_ARGUMENT;
  if (_dmabuf_formats[f].packed &&
      ((unsigned)desc->ycbcr_model > RN_VK_YCBCR_BT2020 ||
       (unsigned)desc->ycbcr_range > RN_VK_YCBCR_FULL))
    return RN_ERROR_INVALID_ARGUMENT;
  struct rn_vk_conversion *conversion;
  rn_result_t conversion_result =
      _dmabuf_conversion(ctx, &_dmabuf_formats[f], desc, &conversion);
  if (conversion_result != RN_OK)
    return conversion_result;

  VkSubresourceLayout planes[RN_VK_DMABUF_MAX_PLANES] = {0};

  for (uint32_t i = 0; i < desc->plane_count; ++i) {
    planes[i].offset = desc->planes[i].offset;
    planes[i].rowPitch = desc->planes[i].stride;
  }

  VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
      .sType =
          VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = desc->modifier,
      .drmFormatModifierPlaneCount = desc->plane_count,
      .pPlaneLayouts = planes,
  };
  VkExternalMemoryImageCreateInfo external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &modifier,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  bool disjoint = false;
  for (uint32_t i = 1; i < desc->plane_count; ++i) {
    struct stat plane;
    if (fstat(desc->planes[i].fd, &plane) < 0)
      return RN_ERROR_INVALID_ARGUMENT;
    disjoint |= plane.st_dev != stat->st_dev || plane.st_ino != stat->st_ino;
  }
  VkImageCreateFlags flags = disjoint ? VK_IMAGE_CREATE_DISJOINT_BIT : 0;
  if (!_dmabuf_importable(ctx, _dmabuf_formats[f].vk, desc->modifier, flags,
                          NULL, desc))
    return RN_ERROR_INVALID_ARGUMENT;
  VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = flags,
      .pNext = &external,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = _dmabuf_formats[f].vk,
      .extent = {desc->width, desc->height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkImage native = VK_NULL_HANDLE;
  VkDeviceMemory memory[RN_VK_DMABUF_MAX_PLANES] = {0};
  int sync_fds[RN_VK_DMABUF_MAX_PLANES] = {-1, -1, -1, -1};
  uint32_t memory_count = disjoint ? desc->plane_count : 1;
  rn_result_t result =
      _result(ctx, vkCreateImage(ctx->device, &info, NULL, &native),
              "vkCreateImage(dmabuf)");

  if (result != RN_OK)
    return result;

  PFN_vkGetImageMemoryRequirements2KHR get_requirements =
      (PFN_vkGetImageMemoryRequirements2KHR)vkGetDeviceProcAddr(
          ctx->device, "vkGetImageMemoryRequirements2KHR");
  PFN_vkBindImageMemory2KHR bind_memory =
      (PFN_vkBindImageMemory2KHR)vkGetDeviceProcAddr(ctx->device,
                                                     "vkBindImageMemory2KHR");
  if (!get_requirements || !bind_memory) {
    result = RN_ERROR_BACKEND;
    goto fail;
  }
  VkBindImageMemoryInfo binds[RN_VK_DMABUF_MAX_PLANES] = {0};
  VkBindImagePlaneMemoryInfo bind_planes[RN_VK_DMABUF_MAX_PLANES] = {0};
  for (uint32_t i = 0; i < memory_count; ++i) {
    VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i;
    VkImagePlaneMemoryRequirementsInfo plane = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
        .planeAspect = aspect};
    VkImageMemoryRequirementsInfo2 req_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = disjoint ? &plane : NULL,
        .image = native};
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    get_requirements(ctx->device, &req_info, &requirements);
    VkMemoryFdPropertiesKHR properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    result =
        _result(ctx,
                ctx->fd_properties(
                    ctx->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                    desc->planes[i].fd, &properties),
                "vkGetMemoryFdPropertiesKHR");
    if (result != RN_OK)
      goto fail;
    struct stat plane_stat;
    uint32_t type = 0;
    uint32_t bits = properties.memoryTypeBits &
                    requirements.memoryRequirements.memoryTypeBits;
    while (type < 32 && !(bits & (1u << type)))
      ++type;
    if (type == 32 || fstat(desc->planes[i].fd, &plane_stat) < 0 ||
        plane_stat.st_size <= 0 ||
        (uint64_t)plane_stat.st_size < requirements.memoryRequirements.size) {
      result = RN_ERROR_INVALID_ARGUMENT;
      goto fail;
    }
    /* Vulkan takes ownership on success; the caller retains its original FD. */
    int import_fd = fcntl(desc->planes[i].fd, F_DUPFD_CLOEXEC, 0);
    if (import_fd < 0) {
      result = RN_ERROR_BACKEND;
      goto fail;
    }
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = native};
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = disjoint ? NULL : &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = import_fd};
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import,
        .allocationSize = plane_stat.st_size,
        .memoryTypeIndex = type};
    result = _result(
        ctx, vkAllocateMemory(ctx->device, &allocation, NULL, &memory[i]),
        "vkAllocateMemory(dmabuf)");
    if (result != RN_OK) {
      close(import_fd);
      goto fail;
    }
    bind_planes[i] = (VkBindImagePlaneMemoryInfo){
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
        .planeAspect = aspect};
    binds[i] = (VkBindImageMemoryInfo){
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .pNext = disjoint ? &bind_planes[i] : NULL,
        .image = native,
        .memory = memory[i]};
    sync_fds[i] = fcntl(desc->planes[i].fd, F_DUPFD_CLOEXEC, 0);
    if (sync_fds[i] < 0) {
      result = RN_ERROR_BACKEND;
      goto fail;
    }
  }
  result = _result(ctx, bind_memory(ctx->device, memory_count, binds),
                   "vkBindImageMemory2KHR(dmabuf)");
  if (result != RN_OK)
    goto fail;

  result = _wrap_vk_image(ctx, native, _dmabuf_formats[f].vk, desc->width,
                          desc->height, _dmabuf_formats[f].opaque,
                          conversion ? NULL : &_dmabuf_formats[f].components,
                          conversion, image);

  if (result != RN_OK)
    goto fail;

  struct rn_vk_texture *texture = _texture(ctx, image->texture);
  if (_dmabuf_formats[f].packed) {
    uint32_t depth =
        _dmabuf_formats[f].vk == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 10
        : _dmabuf_formats[f].vk == VK_FORMAT_R16G16B16A16_UNORM     ? 16
                                                                    : 8;
    if (desc->format == RN_FOURCC('Y', '4', '1', '2'))
      depth = 12;
    texture->packed_yuv = 1u | ((uint32_t)desc->ycbcr_model << 1) |
                          (desc->ycbcr_range == RN_VK_YCBCR_FULL ? 8u : 0u) |
                          (depth << 4);
  }
  texture->dmabuf = true;
  texture->dmabuf_memory_count = memory_count;
  memcpy(texture->dmabuf_memory, memory, sizeof(memory));
  memcpy(texture->dmabuf_fd, sync_fds, sizeof(sync_fds));
  texture->acquire_fd = -1;
  texture->dmabuf_device = stat->st_dev;
  texture->dmabuf_inode = stat->st_ino;
  texture->dmabuf_desc = *desc;
  image->premultiplied = desc->premultiplied;
  return RN_OK;

fail:
  vkDestroyImage(ctx->device, native, NULL);

  for (uint32_t i = 0; i < memory_count; ++i) {
    if (memory[i])
      vkFreeMemory(ctx->device, memory[i], NULL);
    if (sync_fds[i] >= 0)
      close(sync_fds[i]);
  }
  return result;
}

rn_result_t rn_vk_image_acquire_dmabuf(struct rn_state_t *state,
                                       const struct rn_image_t *image,
                                       int acquire_fence_fd) {
  struct rn_vk_context *ctx = _state_context(state);
  if (!ctx || !image || acquire_fence_fd < -1)
    return RN_ERROR_INVALID_ARGUMENT;
  if (ctx->frame_active || ctx->fatal)
    return RN_ERROR_BAD_STATE;
  struct rn_vk_texture *texture = _texture(ctx, image->texture);
  if (!texture || !texture->dmabuf || image->storage != RN_IMAGE_DEDICATED)
    return RN_ERROR_INVALID_ARGUMENT;
  return _dmabuf_acquire(ctx, image->texture, acquire_fence_fd);
}

rn_result_t rn_vk_image_from_dmabuf(struct rn_state_t *state,
                                    const struct rn_vk_dmabuf_desc_t *desc,
                                    struct rn_image_t *image) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || !desc || !image || !desc->width || !desc->height ||
      desc->acquire_fence_fd < -1 || !desc->plane_count ||
      desc->plane_count > RN_VK_DMABUF_MAX_PLANES ||
      desc->width > ctx->properties.limits.maxImageDimension2D ||
      desc->height > ctx->properties.limits.maxImageDimension2D)
    return RN_ERROR_INVALID_ARGUMENT;

  if (ctx->frame_active || ctx->fatal)
    return RN_ERROR_BAD_STATE;

  if (!ctx->dmabuf_enabled)
    return RN_ERROR_BACKEND;

  bool supported = false;

  /* check if description format is supported */
  for (size_t i = 0; i < ctx->dmabuf_format_count; ++i) {
    const struct rn_vk_dmabuf_format_t *format = &ctx->dmabuf_formats[i];

    if (format->format == desc->format && format->modifier == desc->modifier &&
        format->plane_count == desc->plane_count) {
      supported = true;
      break;
    }
  }

  if (!supported)
    return RN_ERROR_INVALID_ARGUMENT;

  struct stat stat;

  if (fstat(desc->planes[0].fd, &stat) < 0)
    return RN_ERROR_INVALID_ARGUMENT;

  /* validate planes */
  for (uint32_t i = 0; i < desc->plane_count; ++i) {
    struct stat plane;

    if (!desc->planes[i].stride || fstat(desc->planes[i].fd, &plane) < 0)
      return RN_ERROR_INVALID_ARGUMENT;
  }

  bool created = !image->width && !image->height;
  rn_result_t result;

  if (created) {
    result = _dmabuf_create(ctx, desc, &stat, image);

    if (result != RN_OK)
      return result;
  } else {
    struct rn_vk_texture *texture = _texture(ctx, image->texture);

    if (!texture || !texture->dmabuf || image->storage != RN_IMAGE_DEDICATED ||
        texture->dmabuf_device != (uint64_t)stat.st_dev ||
        texture->dmabuf_inode != (uint64_t)stat.st_ino ||
        texture->width != desc->width || texture->height != desc->height ||
        texture->dmabuf_desc.format != desc->format ||
        texture->dmabuf_desc.modifier != desc->modifier ||
        texture->dmabuf_desc.plane_count != desc->plane_count ||
        texture->dmabuf_desc.ycbcr_model != desc->ycbcr_model ||
        texture->dmabuf_desc.ycbcr_range != desc->ycbcr_range ||
        texture->dmabuf_desc.chroma_x != desc->chroma_x ||
        texture->dmabuf_desc.chroma_y != desc->chroma_y)
      return RN_ERROR_INVALID_ARGUMENT;

    for (uint32_t i = 0; i < desc->plane_count; ++i) {
      struct stat original, incoming;
      uint32_t memory_index = texture->dmabuf_memory_count == 1 ? 0 : i;
      if (fstat(texture->dmabuf_fd[memory_index], &original) < 0 ||
          fstat(desc->planes[i].fd, &incoming) < 0 ||
          original.st_dev != incoming.st_dev ||
          original.st_ino != incoming.st_ino)
        return RN_ERROR_INVALID_ARGUMENT;
      if (texture->dmabuf_desc.planes[i].offset != desc->planes[i].offset ||
          texture->dmabuf_desc.planes[i].stride != desc->planes[i].stride)
        return RN_ERROR_INVALID_ARGUMENT;
    }
  }

  result = _dmabuf_acquire(ctx, image->texture, desc->acquire_fence_fd);

  if (result != RN_OK && created) {
    _destroy_texture(ctx, image->texture);
    memset(image, 0, sizeof(*image));
  }

  if (result == RN_OK)
    image->premultiplied = desc->premultiplied;

  return result;
}

#else
rn_result_t rn_vk_image_acquire_dmabuf(struct rn_state_t *state,
                                       const struct rn_image_t *image,
                                       int acquire_fence_fd) {
  (void)state;
  (void)image;
  (void)acquire_fence_fd;
  return RN_ERROR_BACKEND;
}

static rn_result_t _dmabuf_release_texture(struct rn_vk_context *ctx,
                                           struct rn_vk_texture *texture) {
  (void)ctx;
  (void)texture;
  return RN_OK;
}

static rn_result_t _dmabuf_init(struct rn_vk_context *ctx) {
  (void)ctx;
  return RN_OK;
}
static rn_result_t _dmabuf_release_pending(struct rn_vk_context *ctx) {
  (void)ctx;
  return RN_OK;
}
static rn_result_t _dmabuf_acquire_commands(struct rn_vk_context *ctx,
                                            struct rn_vk_frame *frame) {
  (void)ctx;
  (void)frame;
  return RN_OK;
}
static void _dmabuf_release_commands(struct rn_vk_context *ctx,
                                     VkCommandBuffer command) {
  (void)ctx;
  (void)command;
}
static void _dmabuf_forget(struct rn_vk_context *ctx,
                           rn_texture_handle_t handle) {
  (void)ctx;
  (void)handle;
}
static void _dmabuf_close(struct rn_vk_texture *texture) { (void)texture; }
rn_result_t rn_vk_image_from_dmabuf(struct rn_state_t *state,
                                    const struct rn_vk_dmabuf_desc_t *desc,
                                    struct rn_image_t *image) {
  (void)state;
  (void)desc;
  (void)image;
  return RN_ERROR_BACKEND;
}
#endif

rn_result_t rn_vk_dmabuf_formats(struct rn_state_t *state,
                                 const struct rn_vk_dmabuf_format_t **formats,
                                 size_t *count) {
  struct rn_vk_context *ctx = _state_context(state);

  if (!ctx || !formats || !count)
    return RN_ERROR_INVALID_ARGUMENT;

  *formats = ctx->dmabuf_formats;
  *count = ctx->dmabuf_format_count;
  return RN_OK;
}
