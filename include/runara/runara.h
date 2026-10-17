#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rn_box_u32_t {
  uint32_t x, y, width, height;
};

struct rn_state_t;

typedef void *(*rn_alloc_fn_t)(size_t size, void *ctx);
typedef void (*rn_free_fn_t)(void *ptr, void *ctx);

struct rn_allocator_t {
  rn_alloc_fn_t alloc;
  rn_free_fn_t free;
  void *ctx;
};

struct rn_log_options_t {
  bool verbose;
  bool quiet;
  bool logfile;
};

struct rn_init_options_t {
  struct rn_allocator_t allocator;
  struct rn_log_options_t log;
};

struct rn_state_t;

struct rn_state_t *rn_state_init(const struct rn_init_options_t *options);

void rn_state_fini(struct rn_state_t *state);

void rn_canvas_set_size(const struct rn_box_u32_t *canvas);
