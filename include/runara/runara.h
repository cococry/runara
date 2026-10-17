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

struct rn_state_t;

struct rn_state_t *rn_state_init(const struct rn_init_options_t *options);

void rn_state_fini(struct rn_state_t *state);

void rn_canvas_set_size(const struct rn_box_u32_t *canvas);
