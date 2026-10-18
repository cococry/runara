
#pragma once

#include "src/log.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <runara/runara.h>

#define RN_INVALID_HANDLE UINT32_MAX

typedef enum rn_buffer_target_t {
  RN_BUFFER_VERTEX,
  RN_BUFFER_INDEX
} rn_buffer_target_t;

typedef enum rn_buffer_usage_t {
  RN_BUFFER_STATIC,
  RN_BUFFER_DYNAMIC,
  RN_BUFFER_STAGING
} rn_buffer_usage_t;

struct rn_buffer_desc_t {
  rn_buffer_target_t target;
  rn_buffer_usage_t usage;
  size_t size;
  const void *data;
};

typedef enum rn_texture_format_t { RN_TEXTURE_RGBA8 } rn_texture_format_t;

struct rn_texture_desc_t {
  uint32_t width;
  uint32_t height;
  rn_texture_format_t format;
};

struct rn_texture_array_desc_t {
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  rn_texture_format_t format;
};

struct rn_texture_array_write_t {
  rn_texture_handle_t texture;
  uint32_t x;
  uint32_t y;
  uint32_t layer;
  uint32_t width;
  uint32_t height;
};

enum rn_shader_pipeline_t {
  RN_PIPELINE_UI_QUAD_ATLAS,
  RN_PIPELINE_UI_QUAD_DEDICATED
};

enum rn_vertex_format_t { RN_VERTEX_FLOAT32X2, RN_VERTEX_FLOAT32X4 };

enum rn_vertex_step_mode_t { RN_STEP_VERTEX, RN_STEP_INSTANCE };

struct rn_vertex_attribute_t {
  uint32_t location;
  uint32_t offset;
  enum rn_vertex_format_t format;
};

struct rn_vertex_buffer_layout_t {
  uint32_t binding;
  uint32_t stride;
  enum rn_vertex_step_mode_t step_mode;
  const struct rn_vertex_attribute_t *attributes;
  size_t attribute_count;
};

enum rn_uniform_type_t {
  RN_UNIFORM_VEC2,
  RN_UNIFORM_SAMPLER_2D_ARRAY,
  RN_UNIFORM_SAMPLER_2D
};

enum rn_shader_stage_t { RN_SHADER_VERTEX, RN_SHADER_FRAGMENT };

struct rn_uniform_binding_t {
  const char *name;
  enum rn_uniform_type_t type;
  enum rn_shader_stage_t stage;
  uint32_t binding;
  float f_data[4];
};

enum rn_blend_mode_t {
  RN_BLEND_OPAQUE,
  RN_BLEND_STRAIGHT_ALPHA,
  RN_BLEND_PREMULTIPLIED_ALPHA
};

struct rn_pipeline_desc_t {
  enum rn_shader_pipeline_t shader;
  enum rn_blend_mode_t blend;
  bool depth_test;

  const struct rn_vertex_buffer_layout_t *vertex_bindings;
  size_t vertex_binding_count;

  const struct rn_uniform_binding_t *uniform_bindings;
  size_t uniform_binding_count;
};

struct rn_draw_indexed_instanced_t {
  uint32_t index_count;
  uint32_t index_offset;
  int32_t vertex_offset;
  uint32_t instance_count;
  uint32_t instance_offset;
};

struct rn_backend_impl_t {
  void *ctx;

  /* Frame lifecycle */
  rn_result_t (*begin_frame)(void *ctx, uint32_t x, uint32_t y, uint32_t width,
                             uint32_t height);

  rn_result_t (*clear_color)(void *ctx, rn_color_t color);

  rn_result_t (*end_frame)(void *ctx);

  /* Buffers */
  rn_result_t (*create_buffer)(void *ctx, const struct rn_buffer_desc_t *desc,
                               rn_buffer_handle_t *out);

  void (*destroy_buffer)(void *ctx, rn_buffer_handle_t buffer);

  rn_result_t (*write_buffer)(void *ctx, rn_buffer_handle_t buffer,
                              uint32_t binding, size_t offset, const void *data,
                              size_t size);

  /* Pipelines */
  rn_result_t (*create_pipeline)(void *ctx,
                                 const struct rn_pipeline_desc_t *desc,
                                 rn_pipeline_handle_t *out);

  void (*destroy_pipeline)(void *ctx, rn_pipeline_handle_t pipeline);

  rn_result_t (*set_pipeline)(void *ctx, rn_pipeline_handle_t pipeline);

  rn_result_t (*set_vertex_buffer)(void *ctx, rn_buffer_handle_t buffer,
                                   uint32_t binding);

  rn_result_t (*set_index_buffer)(void *ctx, rn_buffer_handle_t buffer);

  rn_result_t (*set_uniform_2f)(void *ctx, rn_pipeline_handle_t pipeline,
                                const char *name, float x, float y);

  rn_result_t (*draw_indexed_instanced)(
      void *ctx, const struct rn_draw_indexed_instanced_t *draw);

  /* Textures */
  rn_result_t (*create_texture)(void *ctx, const struct rn_texture_desc_t *desc,
                                const void *rgba_pixels, size_t size,
                                rn_texture_handle_t *out);

  rn_result_t (*create_texture_array)(
      void *ctx, const struct rn_texture_array_desc_t *desc,
      rn_texture_handle_t *out);

  rn_result_t (*write_texture_array_layer)(
      void *ctx, const struct rn_texture_array_write_t *write,
      const void *rgba_pixels, size_t size);

  void (*destroy_texture)(void *ctx, rn_texture_handle_t texture);

  rn_result_t (*set_texture)(void *ctx, uint32_t slot,
                             rn_texture_handle_t texture);
};

bool rn_backend_implement(struct rn_backend_impl_t *impl,
                          enum rn_backend_type_t type,
                          const struct rn_allocator_t *alloc,
                          const struct rn_log_t *log, const void *backend_info);

#if defined(RN_HAS_OPENGL) && RN_HAS_OPENGL
bool rn_gl_backend_implement(struct rn_backend_impl_t *impl,
                             const struct rn_allocator_t *alloc,
                             const struct rn_log_t *log, const void *backend_info);
#endif

#if defined(RN_HAS_VULKAN) && RN_HAS_VULKAN
bool rn_vk_backend_implement(struct rn_backend_impl_t *impl,
                             const struct rn_allocator_t *alloc,
                             const struct rn_log_t *log, const void *backend_info);
#endif

void rn_backend_fini(struct rn_backend_impl_t *impl,
                     enum rn_backend_type_t type,
                     const struct rn_allocator_t *alloc);

#if defined(RN_HAS_OPENGL) && RN_HAS_OPENGL
void rn_gl_backend_fini(struct rn_backend_impl_t *impl,
                        const struct rn_allocator_t *alloc);
#endif

#if defined(RN_HAS_VULKAN) && RN_HAS_VULKAN
void rn_vk_backend_fini(struct rn_backend_impl_t *impl,
                        const struct rn_allocator_t *alloc);
#endif
