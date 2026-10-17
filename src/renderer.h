#pragma once

#include "runara/runara.h"
#include "vendor/linesky/linesky.h"
#include <stdbool.h>
#include <stdint.h>

#define RN_RENDERER_MAX_INSTANCES 16384u
#define RN_RENDERER_MAX_TEXTURE_SLOTS 16u
#define RN_RENDERER_ATLAS_WIDTH 1024u
#define RN_RENDERER_ATLAS_HEIGHT 1024u
#define RN_RENDERER_ATLAS_LAYERS 16u
#define RN_RENDERER_ATLAS_PADDING 1u

struct rn_quad_vertex_t {
  float position[2];
  float uv[2];
};

struct rn_quad_instance_t {
  float rect[4];
  float color[4];
  float uv[4];
  float params[4];
};

struct rn_draw_batch_t {
  rn_pipeline_handle_t pipeline;
  rn_texture_handle_t textures[RN_RENDERER_MAX_TEXTURE_SLOTS];
  uint32_t texture_count;
  uint32_t first_instance;
  uint32_t instance_count;
};

struct rn_renderer_t {
  rn_buffer_handle_t vertex_buffer;
  rn_buffer_handle_t index_buffer;
  rn_buffer_handle_t instance_buffer;
  rn_pipeline_handle_t atlas_pipeline;
  rn_pipeline_handle_t dedicated_pipeline;
  rn_texture_handle_t atlas_texture;

  struct rn_quad_instance_t *instances;
  struct rn_draw_batch_t *batches;
  uint32_t instance_count;
  uint32_t batch_count;
  struct rn_box_u32_t canvas;
  bool frame_active;
  bool initialized;

  ls_atlas2d *atlas_layers;
  ls_vec2d *atlas_scratch;
};

rn_result_t rn_renderer_init(struct rn_state_t *state);
void rn_renderer_fini(struct rn_state_t *state);
