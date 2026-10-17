
#define LINESKY_IMPLEMENTATION
#include "vendor/linesky/linesky.h"

#include "src/internal.h"
#include "src/log.h"
#include <stdlib.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RN_SUBSYS_NAME "RENDERER"

#define RN_RENDERER_CHECK(result)                                              \
  do {                                                                         \
    rn_result_t check_result = (result);                                       \
    if (check_result != RN_OK)                                                 \
      return check_result;                                                     \
  } while (0)

static rn_result_t _submit_instance(struct rn_state_t *state,
                                    const struct rn_quad_instance_t *instance,
                                    rn_pipeline_handle_t pipeline,
                                    rn_texture_handle_t texture);
static rn_result_t _configure_draw(struct rn_state_t *state,
                                   rn_pipeline_handle_t pipeline);
static bool _valid_pixels(uint32_t width, uint32_t height, size_t byte_count);

static const struct rn_quad_vertex_t _quad_vertices[4] = {
    {{0.0f, 0.0f}, {0.0f, 0.0f}},
    {{1.0f, 0.0f}, {1.0f, 0.0f}},
    {{1.0f, 1.0f}, {1.0f, 1.0f}},
    {{0.0f, 1.0f}, {0.0f, 1.0f}},
};

static const uint32_t _quad_indices[6] = {0, 1, 2, 0, 2, 3};

static const struct rn_vertex_attribute_t _vertex_attributes[] = {
    {.location = 0, .offset = 0, .format = RN_VERTEX_FLOAT32X2},
    {.location = 1, .offset = 8, .format = RN_VERTEX_FLOAT32X2},
};

static const struct rn_vertex_attribute_t _instance_attributes[] = {
    {.location = 2, .offset = 0, .format = RN_VERTEX_FLOAT32X4},
    {.location = 3, .offset = 16, .format = RN_VERTEX_FLOAT32X4},
    {.location = 4, .offset = 32, .format = RN_VERTEX_FLOAT32X4},
    {.location = 5, .offset = 48, .format = RN_VERTEX_FLOAT32X4},
};

static const struct rn_vertex_buffer_layout_t _layouts[] = {
    {.binding = 0,
     .stride = sizeof(struct rn_quad_vertex_t),
     .step_mode = RN_STEP_VERTEX,
     .attributes = _vertex_attributes,
     .attribute_count =
         sizeof(_vertex_attributes) / sizeof(_vertex_attributes[0])},
    {.binding = 1,
     .stride = sizeof(struct rn_quad_instance_t),
     .step_mode = RN_STEP_INSTANCE,
     .attributes = _instance_attributes,
     .attribute_count =
         sizeof(_instance_attributes) / sizeof(_instance_attributes[0])},
};

static rn_result_t _submit_instance(struct rn_state_t *state,
                                    const struct rn_quad_instance_t *instance,
                                    rn_pipeline_handle_t pipeline,
                                    rn_texture_handle_t texture) {
  struct rn_renderer_t *renderer = &state->renderer;

  if (!renderer->frame_active)
    return RN_ERROR_BAD_STATE;

  if (renderer->instance_count >= RN_RENDERER_MAX_INSTANCES)
    return RN_ERROR_OVERFLOW;

  struct rn_draw_batch_t *batch = NULL;
  uint32_t texture_slot = 0;

  if (renderer->batch_count) {
    struct rn_draw_batch_t *last =
        &renderer->batches[renderer->batch_count - 1];

    if (last->pipeline == pipeline) {
      /* merge batches by pipeline */
      if (pipeline == renderer->atlas_pipeline) {
        /* atlas instances all share the same texture, so we can just merge */
        batch = last;
      } else {
        /* find if we already have the texture in the last batch */
        for (uint32_t slot = 0; slot < last->texture_count; ++slot) {
          if (last->textures[slot] == texture) {
            batch = last;
            texture_slot = slot;
            break;
          }
        }

        /* insert the texture into the last batch if we have space */
        if (!batch && last->texture_count < RN_RENDERER_MAX_TEXTURE_SLOTS) {
          texture_slot = last->texture_count;
          last->textures[last->texture_count++] = texture;
          batch = last;
        }
      }
    }
  }

  if (!batch) {
    if (renderer->batch_count >= RN_RENDERER_MAX_INSTANCES)
      return RN_ERROR_OVERFLOW;

    /* make new batch */
    batch = &renderer->batches[renderer->batch_count++];
    *batch = (struct rn_draw_batch_t){
        .pipeline = pipeline,
        .first_instance = renderer->instance_count,
    };

    if (pipeline == renderer->dedicated_pipeline) {
      batch->textures[0] = texture;
      batch->texture_count = 1;
    } else {
      batch->textures[0] = renderer->atlas_texture;
      batch->texture_count = 1;
    }
  }

  struct rn_quad_instance_t queued = *instance;

  /* params.x selects the sampler2D slot for dedicated images. */
  if (pipeline == renderer->dedicated_pipeline)
    queued.params[0] = (float)texture_slot;

  renderer->instances[renderer->instance_count++] = queued;
  ++batch->instance_count;

  return RN_OK;
}

static rn_result_t _configure_draw(struct rn_state_t *state,
                                   rn_pipeline_handle_t pipeline) {
  struct rn_backend_impl_t *backend = &state->backend;
  struct rn_renderer_t *renderer = &state->renderer;

  RN_RENDERER_CHECK(backend->set_pipeline(backend->ctx, pipeline));

  /* The GL backend clears its tracked buffer bindings on set_pipeline(). */
  RN_RENDERER_CHECK(
      backend->set_vertex_buffer(backend->ctx, renderer->vertex_buffer, 0));
  RN_RENDERER_CHECK(
      backend->set_vertex_buffer(backend->ctx, renderer->instance_buffer, 1));
  RN_RENDERER_CHECK(
      backend->set_index_buffer(backend->ctx, renderer->index_buffer));

  RN_RENDERER_CHECK(backend->set_uniform_2f(
      backend->ctx, pipeline, "u_screen_size", (float)renderer->canvas.width,
      (float)renderer->canvas.height));

  return RN_OK;
}

static bool _valid_pixels(uint32_t width, uint32_t height, size_t byte_count) {
  if (!width || !height || (size_t)width > SIZE_MAX / 4u / height)
    return false;

  return byte_count >= (size_t)width * height * 4u;
}

rn_result_t rn_renderer_init(struct rn_state_t *state) {
  if (!state)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_renderer_t *renderer = &state->renderer;
  struct rn_backend_impl_t *backend = &state->backend;
  struct rn_allocator_t *allocator = &state->allocator;

  if (renderer->initialized)
    return RN_ERROR_BAD_STATE;

  if (!backend->create_buffer || !backend->create_pipeline ||
      !backend->create_texture_array || !allocator->alloc || !allocator->free)
    return RN_ERROR_BAD_STATE;

  RN_TRACE(
      state->log,
      "initializing 2D renderer (max instances=%u, dedicated texture slots=%u)",
      (unsigned)RN_RENDERER_MAX_INSTANCES,
      (unsigned)RN_RENDERER_MAX_TEXTURE_SLOTS);

  memset(renderer, 0, sizeof(*renderer));
  renderer->vertex_buffer = RN_INVALID_HANDLE;
  renderer->index_buffer = RN_INVALID_HANDLE;
  renderer->instance_buffer = RN_INVALID_HANDLE;
  renderer->atlas_pipeline = RN_INVALID_HANDLE;
  renderer->dedicated_pipeline = RN_INVALID_HANDLE;
  renderer->atlas_texture = RN_INVALID_HANDLE;

  renderer->instances = allocator->alloc(
      sizeof(*renderer->instances) * RN_RENDERER_MAX_INSTANCES, allocator->ctx);
  renderer->batches = allocator->alloc(
      sizeof(*renderer->batches) * RN_RENDERER_MAX_INSTANCES, allocator->ctx);

  if (!renderer->instances || !renderer->batches)
    goto allocation_failed;

  RN_TRACE(state->log, "allocated instance and draw batch storage");

  const struct rn_buffer_desc_t vertex_desc = {.target = RN_BUFFER_VERTEX,
                                               .usage = RN_BUFFER_STATIC,
                                               .size = sizeof(_quad_vertices),
                                               .data = _quad_vertices};
  const struct rn_buffer_desc_t index_desc = {.target = RN_BUFFER_INDEX,
                                              .usage = RN_BUFFER_STATIC,
                                              .size = sizeof(_quad_indices),
                                              .data = _quad_indices};
  const struct rn_buffer_desc_t instance_desc = {
      .target = RN_BUFFER_VERTEX,
      .usage = RN_BUFFER_DYNAMIC,
      .size = sizeof(*renderer->instances) * RN_RENDERER_MAX_INSTANCES,
      .data = NULL};

  rn_result_t result = backend->create_buffer(backend->ctx, &vertex_desc,
                                              &renderer->vertex_buffer);
  if (result != RN_OK)
    goto failed;

  result = backend->create_buffer(backend->ctx, &index_desc,
                                  &renderer->index_buffer);
  if (result != RN_OK)
    goto failed;

  result = backend->create_buffer(backend->ctx, &instance_desc,
                                  &renderer->instance_buffer);
  if (result != RN_OK)
    goto failed;

  RN_TRACE(state->log,
           "created shared quad buffers (vertex=%u, index=%u, instances=%u)",
           (unsigned)renderer->vertex_buffer, (unsigned)renderer->index_buffer,
           (unsigned)renderer->instance_buffer);

  const struct rn_uniform_binding_t atlas_uniforms[] = {
      {.name = "u_texture_array",
       .type = RN_UNIFORM_SAMPLER_2D_ARRAY,
       .stage = RN_SHADER_FRAGMENT,
       .binding = 0},
      {.name = "u_screen_size",
       .type = RN_UNIFORM_VEC2,
       .stage = RN_SHADER_VERTEX},
  };

  /* Each sampler is assigned to its matching texture unit at creation. */
  struct rn_uniform_binding_t
      dedicated_uniforms[RN_RENDERER_MAX_TEXTURE_SLOTS + 1] = {0};
  char sampler_names[RN_RENDERER_MAX_TEXTURE_SLOTS][24];

  for (uint32_t slot = 0; slot < RN_RENDERER_MAX_TEXTURE_SLOTS; ++slot) {
    snprintf(sampler_names[slot], sizeof(sampler_names[slot]), "u_textures[%u]",
             (unsigned)slot);

    dedicated_uniforms[slot] = (struct rn_uniform_binding_t){
        .name = sampler_names[slot],
        .type = RN_UNIFORM_SAMPLER_2D,
        .stage = RN_SHADER_FRAGMENT,
        .binding = slot,
    };
  }

  dedicated_uniforms[RN_RENDERER_MAX_TEXTURE_SLOTS] =
      (struct rn_uniform_binding_t){
          .name = "u_screen_size",
          .type = RN_UNIFORM_VEC2,
          .stage = RN_SHADER_VERTEX,
      };

  const struct rn_pipeline_desc_t atlas_desc = {
      .shader = RN_PIPELINE_UI_QUAD_ATLAS,
      .blend = RN_BLEND_STRAIGHT_ALPHA,
      .depth_test = false,
      .vertex_bindings = _layouts,
      .vertex_binding_count = sizeof(_layouts) / sizeof(_layouts[0]),
      .uniform_bindings = atlas_uniforms,
      .uniform_binding_count =
          sizeof(atlas_uniforms) / sizeof(atlas_uniforms[0]),
  };

  const struct rn_pipeline_desc_t dedicated_desc = {
      .shader = RN_PIPELINE_UI_QUAD_DEDICATED,
      .blend = RN_BLEND_STRAIGHT_ALPHA,
      .depth_test = false,
      .vertex_bindings = _layouts,
      .vertex_binding_count = sizeof(_layouts) / sizeof(_layouts[0]),
      .uniform_bindings = dedicated_uniforms,
      .uniform_binding_count =
          sizeof(dedicated_uniforms) / sizeof(dedicated_uniforms[0]),
  };

  result = backend->create_pipeline(backend->ctx, &atlas_desc,
                                    &renderer->atlas_pipeline);
  if (result != RN_OK)
    goto failed;

  result = backend->create_pipeline(backend->ctx, &dedicated_desc,
                                    &renderer->dedicated_pipeline);
  if (result != RN_OK)
    goto failed;

  RN_TRACE(
      state->log,
      "created atlas and dedicated-texture pipelines (atlas=%u, dedicated=%u)",
      (unsigned)renderer->atlas_pipeline,
      (unsigned)renderer->dedicated_pipeline);

  const struct rn_texture_array_desc_t texture_desc = {
      .width = RN_RENDERER_ATLAS_WIDTH,
      .height = RN_RENDERER_ATLAS_HEIGHT,
      .layers = RN_RENDERER_ATLAS_LAYERS,
      .format = RN_TEXTURE_RGBA8,
  };

  result = backend->create_texture_array(backend->ctx, &texture_desc,
                                         &renderer->atlas_texture);
  if (result != RN_OK)
    goto failed;

  /* linesky supports only up until uint16 dimensions */
  if (RN_RENDERER_ATLAS_WIDTH > UINT16_MAX ||
      RN_RENDERER_ATLAS_HEIGHT > UINT16_MAX)
    return RN_ERROR_INVALID_ARGUMENT;

  /* create linesky atlas layers */
  renderer->atlas_layers = allocator->alloc(sizeof(*renderer->atlas_layers) *
                                                RN_RENDERER_ATLAS_LAYERS,
                                            allocator->ctx);
  if (!renderer->atlas_layers) {
    result = RN_ERROR_OUT_OF_MEMORY;
    goto failed;
  }

  renderer->atlas_scratch = allocator->alloc(
      sizeof(ls_vec2d) * RN_RENDERER_ATLAS_WIDTH, allocator->ctx);

  if (!renderer->atlas_scratch) {
    result = RN_ERROR_OUT_OF_MEMORY;
    goto failed;
  }

  memset(renderer->atlas_layers, 0,
         sizeof(*renderer->atlas_layers) * RN_RENDERER_ATLAS_LAYERS);

  for (uint32_t layer = 0; layer < RN_RENDERER_ATLAS_LAYERS; ++layer) {
    ls_atlas_init(
        &renderer->atlas_layers[layer],
        (ls_vec2d){RN_RENDERER_ATLAS_WIDTH, RN_RENDERER_ATLAS_HEIGHT});

    if (!renderer->atlas_layers[layer]._init) {
      result = RN_ERROR_OUT_OF_MEMORY;
      goto failed;
    }
  }

  renderer->initialized = true;

  RN_TRACE(state->log, "created RGBA8 atlas texture %u (%ux%u, %u layers)",
           (unsigned)renderer->atlas_texture, (unsigned)RN_RENDERER_ATLAS_WIDTH,
           (unsigned)RN_RENDERER_ATLAS_HEIGHT,
           (unsigned)RN_RENDERER_ATLAS_LAYERS);

  RN_TRACE(state->log, "High level 2D renderer ready");

  return RN_OK;

allocation_failed:
  result = RN_ERROR_OUT_OF_MEMORY;
failed:
  RN_TRACE(state->log,
           "renderer initialization failed (result=%d); cleaning up",
           (int)result);
  rn_renderer_fini(state);
  return result;
}

void rn_renderer_fini(struct rn_state_t *state) {
  if (!state)
    return;

  struct rn_renderer_t *renderer = &state->renderer;
  struct rn_backend_impl_t *backend = &state->backend;
  struct rn_allocator_t *allocator = &state->allocator;

  if (renderer->initialized || renderer->instances || renderer->batches)
    RN_TRACE(state->log, "tearing down 2D renderer and owned GPU resources");

  if (backend->destroy_texture && renderer->atlas_texture != RN_INVALID_HANDLE)
    backend->destroy_texture(backend->ctx, renderer->atlas_texture);

  if (backend->destroy_pipeline) {
    if (renderer->atlas_pipeline != RN_INVALID_HANDLE)
      backend->destroy_pipeline(backend->ctx, renderer->atlas_pipeline);
    if (renderer->dedicated_pipeline != RN_INVALID_HANDLE)
      backend->destroy_pipeline(backend->ctx, renderer->dedicated_pipeline);
  }

  if (backend->destroy_buffer) {
    if (renderer->vertex_buffer != RN_INVALID_HANDLE)
      backend->destroy_buffer(backend->ctx, renderer->vertex_buffer);
    if (renderer->index_buffer != RN_INVALID_HANDLE)
      backend->destroy_buffer(backend->ctx, renderer->index_buffer);
    if (renderer->instance_buffer != RN_INVALID_HANDLE)
      backend->destroy_buffer(backend->ctx, renderer->instance_buffer);
  }

  if (allocator->free) {
    if (renderer->instances)
      allocator->free(renderer->instances, allocator->ctx);
    if (renderer->batches)
      allocator->free(renderer->batches, allocator->ctx);
  }

  if (renderer->atlas_layers) {
    for (uint32_t layer = 0; layer < RN_RENDERER_ATLAS_LAYERS; ++layer)
      free(renderer->atlas_layers[layer].skyline);

    allocator->free(renderer->atlas_layers, allocator->ctx);
  }
  if (renderer->atlas_scratch)
    allocator->free(renderer->atlas_scratch, allocator->ctx);

  memset(renderer, 0, sizeof(*renderer));

  RN_TRACE(state->log, "2D renderer teardown complete");
}

rn_result_t rn_begin(struct rn_state_t *state) {
  if (!state)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_renderer_t *renderer = &state->renderer;
  struct rn_backend_impl_t *backend = &state->backend;

  if (!renderer->initialized || renderer->frame_active)
    return RN_ERROR_BAD_STATE;

  RN_RENDERER_CHECK(
      backend->begin_frame(backend->ctx, renderer->canvas.x, renderer->canvas.y,
                           renderer->canvas.width, renderer->canvas.height));

  renderer->instance_count = 0;
  renderer->batch_count = 0;
  renderer->frame_active = true;
  return RN_OK;
}

rn_result_t rn_clear_color(struct rn_state_t *state, struct rn_color_t color) {
  if (!state)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_renderer_t *renderer = &state->renderer;
  struct rn_backend_impl_t *backend = &state->backend;

  if (!renderer->initialized)
    return RN_ERROR_BAD_STATE;

  RN_RENDERER_CHECK(backend->clear_color(backend->ctx, color));

  return RN_OK;
}

rn_result_t rn_draw_rect(struct rn_state_t *state, struct rn_rect_t rect,
                         struct rn_color_t color) {
  if (!state)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_renderer_t *renderer = &state->renderer;
  struct rn_quad_instance_t instance = {
      .rect = {rect.x, rect.y, rect.width, rect.height},
      .color = {color.r, color.g, color.b, color.a},
      .uv = {0.0f, 0.0f, 1.0f, 1.0f},
      .params = {0.0f, 0.0f, 0.0f, 0.0f},
  };

  return _submit_instance(state, &instance, renderer->atlas_pipeline,
                          renderer->atlas_texture);
}

rn_result_t rn_draw_texture(struct rn_state_t *state,
                            const struct rn_image_t *image,
                            struct rn_rect_t rect, struct rn_color_t tint) {
  if (!state || !image || image->texture == RN_INVALID_HANDLE)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_renderer_t *renderer = &state->renderer;

  if (image->storage != RN_IMAGE_ATLAS && image->storage != RN_IMAGE_DEDICATED)
    return RN_ERROR_INVALID_ARGUMENT;

  if (image->storage == RN_IMAGE_ATLAS &&
      image->texture != renderer->atlas_texture)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_quad_instance_t instance = {
      .rect = {rect.x, rect.y, rect.width, rect.height},
      .color = {tint.r, tint.g, tint.b, tint.a},
      .uv = {image->uv[0], image->uv[1], image->uv[2], image->uv[3]},
      .params = {0.0f, 0.0f, (float)image->layer, 2.0f},
  };

  rn_pipeline_handle_t pipeline = image->storage == RN_IMAGE_ATLAS
                                      ? renderer->atlas_pipeline
                                      : renderer->dedicated_pipeline;

  return _submit_instance(state, &instance, pipeline, image->texture);
}

rn_result_t rn_end(struct rn_state_t *state) {
  if (!state)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_renderer_t *renderer = &state->renderer;
  struct rn_backend_impl_t *backend = &state->backend;

  if (!renderer->initialized || !renderer->frame_active)
    return RN_ERROR_BAD_STATE;

  rn_result_t result = RN_OK;

  if (renderer->instance_count) {
    result = backend->write_buffer(
        backend->ctx, renderer->instance_buffer, 1, 0, renderer->instances,
        sizeof(*renderer->instances) * renderer->instance_count);
    if (result != RN_OK)
      goto finish;

    rn_pipeline_handle_t active_pipeline = RN_INVALID_HANDLE;

    for (uint32_t batch_index = 0; batch_index < renderer->batch_count;
         ++batch_index) {
      const struct rn_draw_batch_t *batch = &renderer->batches[batch_index];

      if (batch->pipeline != active_pipeline) {
        result = _configure_draw(state, batch->pipeline);
        if (result != RN_OK)
          goto finish;

        active_pipeline = batch->pipeline;
      }

      for (uint32_t slot = 0; slot < batch->texture_count; ++slot) {
        result =
            backend->set_texture(backend->ctx, slot, batch->textures[slot]);
        if (result != RN_OK)
          goto finish;
      }

      const struct rn_draw_indexed_instanced_t draw = {
          .index_count = 6,
          .index_offset = 0,
          .vertex_offset = 0,
          .instance_count = batch->instance_count,
          .instance_offset = batch->first_instance,
      };

      result = backend->draw_indexed_instanced(backend->ctx, &draw);
      if (result != RN_OK)
        goto finish;
    }
  }

finish: {
  rn_result_t end_result = backend->end_frame(backend->ctx);
  renderer->frame_active = false;

  return result == RN_OK ? end_result : result;
}
}
rn_result_t rn_texture_upload(struct rn_state_t *state, uint32_t width,
                              uint32_t height, const void *rgba,
                              size_t byte_count, struct rn_image_t *image) {
  if (!state || !image || !rgba || !_valid_pixels(width, height, byte_count))
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_backend_impl_t *backend = &state->backend;

  if (!state->renderer.initialized)
    return RN_ERROR_BAD_STATE;

  const struct rn_texture_desc_t desc = {
      .width = width,
      .height = height,
      .format = RN_TEXTURE_RGBA8,
  };
  rn_texture_handle_t texture = RN_INVALID_HANDLE;

  RN_RENDERER_CHECK(
      backend->create_texture(backend->ctx, &desc, rgba, byte_count, &texture));

  RN_TRACE(state->log, "uploaded dedicated RGBA8 texture %u (%ux%u, %zu bytes)",
           (unsigned)texture, (unsigned)width, (unsigned)height,
           (size_t)width * height * 4u);

  *image = (struct rn_image_t){
      .storage = RN_IMAGE_DEDICATED,
      .texture = texture,
      .width = width,
      .height = height,
      .uv = {0.0f, 0.0f, 1.0f, 1.0f},
  };

  return RN_OK;
}

rn_result_t rn_atlas_upload(struct rn_state_t *state, uint32_t width,
                            uint32_t height, const void *rgba,
                            size_t byte_count, struct rn_image_t *image) {
  if (!state || !image || !rgba || !_valid_pixels(width, height, byte_count))
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_renderer_t *renderer = &state->renderer;
  struct rn_backend_impl_t *backend = &state->backend;

  if (!renderer->initialized || !renderer->atlas_layers ||
      !renderer->atlas_scratch)
    return RN_ERROR_BAD_STATE;

  /* Linesky uses uint16_t for its atlas and rectangle dimensions. */
  if (RN_RENDERER_ATLAS_WIDTH > UINT16_MAX ||
      RN_RENDERER_ATLAS_HEIGHT > UINT16_MAX)
    return RN_ERROR_BAD_STATE;

  if (width > RN_RENDERER_ATLAS_WIDTH || height > RN_RENDERER_ATLAS_HEIGHT ||
      RN_RENDERER_ATLAS_PADDING > RN_RENDERER_ATLAS_WIDTH - width ||
      RN_RENDERER_ATLAS_PADDING > RN_RENDERER_ATLAS_HEIGHT - height)
    return RN_ERROR_ATLAS_FULL;

  const ls_vec2d padded_size = {
      .x = (uint16_t)(width + RN_RENDERER_ATLAS_PADDING),
      .y = (uint16_t)(height + RN_RENDERER_ATLAS_PADDING),
  };

  for (uint32_t layer = 0; layer < RN_RENDERER_ATLAS_LAYERS; ++layer) {
    ls_atlas2d *atlas = &renderer->atlas_layers[layer];

    if (!atlas->_init)
      continue;

    /*
     * Copy the current skyline into reusable scratch memory.
     * Only active nodes need to be copied.
     */
    memcpy(renderer->atlas_scratch, atlas->skyline,
           sizeof(ls_vec2d) * atlas->_nskyline);

    ls_atlas2d candidate = *atlas;
    candidate.skyline = renderer->atlas_scratch;

    float position_x = 0.0f;
    float position_y = 0.0f;

    if (!ls_atlas_push_rect(&candidate, &position_x, &position_y, padded_size))
      continue;

    const uint32_t atlas_x = (uint32_t)position_x;
    const uint32_t atlas_y = (uint32_t)position_y;

    const struct rn_texture_array_write_t write = {
        .texture = renderer->atlas_texture,
        .x = atlas_x,
        .y = atlas_y,
        .layer = layer,
        .width = width,
        .height = height,
    };

    rn_result_t result = backend->write_texture_array_layer(
        backend->ctx, &write, rgba, byte_count);

    if (result != RN_OK)
      return result;

    /*
     * The GPU upload succeeded. Commit the candidate skyline
     * to the real atlas.
     */
    memcpy(atlas->skyline, renderer->atlas_scratch,
           sizeof(ls_vec2d) * candidate._nskyline);

    atlas->_nskyline = candidate._nskyline;

    *image = (struct rn_image_t){
        .storage = RN_IMAGE_ATLAS,
        .texture = renderer->atlas_texture,
        .width = width,
        .height = height,
        .layer = layer,
        .uv =
            {
                (float)atlas_x / RN_RENDERER_ATLAS_WIDTH,
                (float)atlas_y / RN_RENDERER_ATLAS_HEIGHT,
                (float)(atlas_x + width) / RN_RENDERER_ATLAS_WIDTH,
                (float)(atlas_y + height) / RN_RENDERER_ATLAS_HEIGHT,
            },
    };

    RN_TRACE(state->log,
             "uploaded %ux%u RGBA8 image to atlas layer %u at (%u, %u)",
             (unsigned)width, (unsigned)height, (unsigned)layer,
             (unsigned)atlas_x, (unsigned)atlas_y);

    return RN_OK;
  }

  RN_TRACE(state->log, "texture atlas is full (%ux%u image)", (unsigned)width,
           (unsigned)height);

  return RN_ERROR_ATLAS_FULL;
}

void rn_texture_release(struct rn_state_t *state, struct rn_image_t *image) {
  if (!state || !image)
    return;

  if (image->storage == RN_IMAGE_DEDICATED &&
      image->texture != RN_INVALID_HANDLE && state->backend.destroy_texture) {
    RN_TRACE(state->log, "releasing dedicated texture %u",
             (unsigned)image->texture);
    state->backend.destroy_texture(state->backend.ctx, image->texture);
  }

  /* Atlas allocations are retained until the whole renderer is destroyed. */
  image->texture = RN_INVALID_HANDLE;
}

void rn_canvas_set_bounds(struct rn_state_t *state,
                          const struct rn_box_u32_t *canvas) {
  if (!state || !canvas)
    return;
  struct rn_renderer_t *renderer = &state->renderer;

  renderer->canvas = *canvas;
}
