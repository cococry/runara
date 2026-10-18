#include "src/backend.h"
#include "runara/runara.h"
#include "runara/vulkan.h"
#include "src/internal.h"
#include "src/log.h"
#include "src/vulkan/shaders.h"

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

struct rn_vk_context {
  VkInstance instance;
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
};

static rn_result_t _result(struct rn_vk_context *ctx, VkResult status,
                           const char *op);
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

/* Frame command buffers and the enclosing render need to be handled by the host
 */
static rn_result_t _begin_frame(void *backend_ctx, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !ctx->command_buffer || !width || !height || ctx->frame_active)
    return RN_ERROR_BAD_STATE;

  ctx->extent_x = x;
  ctx->extent_y = y;
  ctx->extent_width = width;
  ctx->extent_height = height;
  ctx->frame_active = true;
  ctx->pipeline = RN_INVALID_HANDLE;
  ctx->index_buffer = RN_INVALID_HANDLE;
  for (uint32_t binding = 0; binding < RN_VK_MAX_BINDINGS; binding++)
    ctx->vertex_buffers[binding] = RN_INVALID_HANDLE;
  for (uint32_t slot = 0; slot < RN_VK_MAX_TEXTURES; slot++)
    ctx->bound_textures[slot] = RN_INVALID_HANDLE;

  const VkViewport viewport = {(float)x,      (float)y, (float)width,
                               (float)height, 0.0f,     1.0f};
  const VkRect2D scissor = {{(int32_t)x, (int32_t)y}, {width, height}};
  vkCmdSetViewport(ctx->command_buffer, 0, 1, &viewport);
  vkCmdSetScissor(ctx->command_buffer, 0, 1, &scissor);
  return RN_OK;
}

static rn_result_t _clear_color(void *backend_ctx, rn_color_t color) {
  struct rn_vk_context *ctx = backend_ctx;
  if (!ctx || !ctx->frame_active)
    return RN_ERROR_BAD_STATE;

  VkClearAttachment attachment = {0};
  attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  attachment.colorAttachment = 0;
  attachment.clearValue.color.float32[0] = color.r;
  attachment.clearValue.color.float32[1] = color.g;
  attachment.clearValue.color.float32[2] = color.b;
  attachment.clearValue.color.float32[3] = color.a;
  const VkClearRect region = {
      .rect = {{(int32_t)ctx->extent_x, (int32_t)ctx->extent_y},
               {ctx->extent_width, ctx->extent_height}},
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
  ctx->frame_active = false;
  ctx->command_buffer = VK_NULL_HANDLE;
  return RN_OK;
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
  if (!ctx || !desc || !out || !ctx->render_pass ||
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

  /* Vertex layout */
  for (size_t binding_index = 0; binding_index < desc->vertex_binding_count;
       binding_index++) {

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
         attribute_index++) {
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

  /* Atlas uses one combined sampler, dedicated pipelines use max textures. */
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

  /* Shaders */
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

  /* Misc vulkan drawing setup */
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

static rn_result_t _descriptor_for_draw(struct rn_vk_context *ctx,
                                        struct rn_vk_pipeline *pipeline,
                                        VkDescriptorSet *out) {
  /* cached descriptor set retrieval */

  const uint32_t count = pipeline->descriptor_count;
  rn_texture_handle_t textures[RN_VK_MAX_TEXTURES];
  rn_texture_handle_t fallback = ctx->bound_textures[0];

  if (!_texture(ctx, fallback))
    return RN_ERROR_BAD_STATE;

  for (uint32_t slot = 0; slot < count; slot++) {
    rn_texture_handle_t handle = ctx->bound_textures[slot];
    textures[slot] = _texture(ctx, handle) ? handle : fallback;
  }

  for (struct rn_vk_descriptor_entry *entry = pipeline->descriptors; entry;
       entry = entry->next) {
    if (entry->count == count &&
        !memcmp(textures, entry->textures, count * sizeof(*textures))) {
      /* found an already viable descriptor set for the textures */
      *out = entry->set;
      return RN_OK;
    }
  }

  /* allocate new page */

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

  page->used++;

  VkDescriptorImageInfo images[RN_VK_MAX_TEXTURES] = {0};
  for (uint32_t slot = 0; slot < count; slot++) {
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
  vkDestroySampler(ctx->device, texture->sampler, NULL);
  vkDestroyImageView(ctx->device, texture->view, NULL);
  vmaDestroyImage(ctx->vma, texture->image, texture->allocation);
  texture->alive = false;
  _VK_TRACE(ctx->log, "destroyed texture %u", (unsigned)handle);
}

bool rn_vk_backend_implement(struct rn_backend_impl_t *impl,
                             const struct rn_allocator_t *alloc,
                             const struct rn_log_t *log, void *backend_info) {
  const struct rn_vk_device_info_t *info = backend_info;
  if (!impl || !alloc || !alloc->alloc || !alloc->realloc || !alloc->free ||
      !info || !info->instance || !info->physical_device || !info->device ||
      !info->graphics_queue || !info->render_pass)
    return false;

  struct rn_vk_context *ctx = alloc->alloc(sizeof(*ctx), alloc->ctx);
  if (!ctx)
    return false;

  memset(ctx, 0, sizeof(*ctx));
  ctx->instance = info->instance;
  ctx->physical_device = info->physical_device;
  ctx->device = info->device;
  ctx->queue = info->graphics_queue;
  ctx->queue_family = info->graphics_queue_family;
  ctx->render_pass = info->render_pass;
  ctx->alloc = alloc;
  ctx->log = log;
  ctx->pipeline = RN_INVALID_HANDLE;
  ctx->index_buffer = RN_INVALID_HANDLE;

  vkGetPhysicalDeviceProperties(ctx->physical_device, &ctx->properties);
  if (ctx->properties.apiVersion < VK_API_VERSION_1_0 ||
      ctx->properties.limits.maxPerStageDescriptorSamplers <
          RN_VK_MAX_TEXTURES ||
      ctx->properties.limits.maxPerStageDescriptorSampledImages <
          RN_VK_MAX_TEXTURES) {
    _VK_ERROR(log, "Vulkan device cannot provide 16 sampler slots");
    alloc->free(ctx, alloc->ctx);
    return false;
  }

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
  _VK_TRACE(log, "Vulkan 1.0 backend ready (GPU=%s, queue family=%u)",
            ctx->properties.deviceName, ctx->queue_family);
  return true;

failed:
  if (ctx->upload_fence)
    vkDestroyFence(ctx->device, ctx->upload_fence, NULL);
  if (ctx->upload_pool)
    vkDestroyCommandPool(ctx->device, ctx->upload_pool, NULL);
  if (ctx->vma)
    vmaDestroyAllocator(ctx->vma);
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

  /* The host must have waited for any draw submissions that use these
   * resources before tearing down Runara. */
  _VK_TRACE(ctx->log,
            "tearing down Vulkan resources (%zu buffers, %zu textures, %zu "
            "pipelines)",
            ctx->buffer_count, ctx->texture_count, ctx->pipeline_count);

  for (size_t index = 0; index < ctx->pipeline_count; index++)
    if (ctx->pipelines[index].alive)
      _destroy_pipeline(ctx, (rn_pipeline_handle_t)index);

  for (size_t index = 0; index < ctx->texture_count; index++)
    if (ctx->textures[index].alive)
      _destroy_texture(ctx, (rn_texture_handle_t)index);

  for (size_t index = 0; index < ctx->buffer_count; index++)
    if (ctx->buffers[index].alive)
      _destroy_buffer(ctx, (rn_buffer_handle_t)index);

  if (ctx->upload_fence)
    vkDestroyFence(ctx->device, ctx->upload_fence, NULL);
  if (ctx->upload_pool)
    vkDestroyCommandPool(ctx->device, ctx->upload_pool, NULL);
  vmaDestroyAllocator(ctx->vma);
  alloc->free(ctx->buffers, alloc->ctx);
  alloc->free(ctx->textures, alloc->ctx);
  alloc->free(ctx->pipelines, alloc->ctx);
  _VK_TRACE(ctx->log, "Vulkan backend teardown complete");
  alloc->free(ctx, alloc->ctx);
  memset(backend, 0, sizeof(*backend));
}

rn_result_t rn_vk_set_command_buffer(struct rn_state_t *state,
                                     VkCommandBuffer command_buffer) {
  if (!state || !command_buffer ||
      state->backend_type != RN_RENDER_BACKEND_VULKAN)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_backend_impl_t *backend = &state->backend;
  if (backend->create_buffer != _create_buffer || !backend->ctx)
    return RN_ERROR_BAD_STATE;

  struct rn_vk_context *ctx = backend->ctx;
  if (ctx->frame_active)
    return RN_ERROR_BAD_STATE;

  ctx->command_buffer = command_buffer;
  return RN_OK;
}
