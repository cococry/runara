#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct rn_box_u32_t {
  uint32_t x, y, width, height;
};

struct rn_state_t;

typedef void *(*rn_alloc_fn_t)(size_t size, void *ctx);
typedef void (*rn_free_fn_t)(void *ptr, void *ctx);

typedef struct {
    rn_alloc_fn_t alloc;
    rn_free_fn_t free;
    void *ctx;
} rn_allocator_t;

struct rn_state_t *rn_state_init(const rn_allocator_t* alloc);

void rn_canvas_set_size(const struct rn_box_u32_t* canvas);
