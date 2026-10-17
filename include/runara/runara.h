#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rn_color_t {
  float r, g, b, a;
} rn_color_t;

struct rn_box_u32_t {
  uint32_t x, y, width, height;
};

struct rn_box_f32_t {
  float x, y, width, height;
};

struct rn_state_t;

typedef void *(*rn_alloc_fn_t)(size_t size, void *ctx);
typedef void *(*rn_realloc_fn_t)(void *ptr, size_t size, void *ctx);
typedef void (*rn_free_fn_t)(void *ptr, void *ctx);

struct rn_allocator_t {
  rn_alloc_fn_t alloc;
  /* optional */
  rn_realloc_fn_t realloc;
  rn_free_fn_t free;
  void *ctx;
};

struct rn_log_options_t {
  bool verbose;
  bool quiet;
  bool logfile;
};

enum rn_backend_type_t {
  RN_RENDER_BACKEND_OPENGL = 0,
  RN_RENDER_BACKEND_VULKAN
};

typedef void *(*rn_gl_load_proc)(const char *name);

struct rn_init_options_t {
  struct rn_allocator_t allocator;
  struct rn_log_options_t log;

  enum rn_backend_type_t render_backend;

  /* optional, but needs to be supplied for OpenGL */
  rn_gl_load_proc gl_load_proc;
};

struct rn_rect_t {
  float x, y, width, height;
};

enum rn_image_storage_t { RN_IMAGE_DEDICATED, RN_IMAGE_ATLAS };

typedef uint32_t rn_buffer_handle_t;
typedef uint32_t rn_texture_handle_t;
typedef uint32_t rn_pipeline_handle_t;

struct rn_image_t {
  enum rn_image_storage_t storage;
  rn_texture_handle_t texture;
  uint32_t width, height;
  uint32_t layer;
  float uv[4];
};

typedef enum rn_result_t {
  RN_OK = 0,
  RN_ERROR_INVALID_ARGUMENT,
  RN_ERROR_OUT_OF_MEMORY,
  RN_ERROR_BACKEND,
  RN_ERROR_BAD_STATE,
  RN_ERROR_OVERFLOW,
  RN_ERROR_ATLAS_FULL
} rn_result_t;

const char *rn_result_to_str(rn_result_t result);

rn_result_t rn_texture_upload(struct rn_state_t *state, uint32_t width,
                              uint32_t height, const void *rgba,
                              size_t byte_count, struct rn_image_t *image);
rn_result_t rn_atlas_upload(struct rn_state_t *state, uint32_t width,
                            uint32_t height, const void *rgba,
                            size_t byte_count, struct rn_image_t *image);
void rn_texture_release(struct rn_state_t *state, struct rn_image_t *image);

rn_result_t rn_begin(struct rn_state_t *state);

rn_result_t rn_clear_color(struct rn_state_t *state, struct rn_color_t color);

rn_result_t rn_draw_rect(struct rn_state_t *state, struct rn_rect_t rect,
                         struct rn_color_t color);

rn_result_t rn_draw_texture(struct rn_state_t *state,
                            const struct rn_image_t *image,
                            struct rn_rect_t rect, struct rn_color_t tint);
rn_result_t rn_end(struct rn_state_t *state);

struct rn_state_t *rn_state_init(const struct rn_init_options_t *options,
                                 uint32_t init_width, uint32_t init_height);

void rn_state_fini(struct rn_state_t *state);

void rn_canvas_set_bounds(struct rn_state_t *state,
                        const struct rn_box_u32_t *canvas);
