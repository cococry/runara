#include "../include/runara/runara.h"
#include <assert.h>

struct rn_state_t {
  rn_allocator_t allocator;
};

static void *_default_alloc(size_t size, void *ctx);
static void _default_free(void *ptr, void *ctx);

static void *_default_alloc(size_t size, void *ctx) {
  (void)ctx;
  return malloc(size);
}

static void _default_free(void *ptr, void *ctx) {
  (void)ctx;
  free(ptr);
}

struct rn_state_t *rn_state_init(const rn_allocator_t *allocator) {
  rn_allocator_t a = allocator ? *allocator
                               : (rn_allocator_t){
                                     .alloc = _default_alloc,
                                     .free = _default_free,
                                     .ctx = NULL,
                                 };

  if (!a.alloc || !a.free)
    return NULL;

  struct rn_state_t *state = a.alloc(sizeof(*state), a.ctx);

  if (!state)
    return NULL;

  state->allocator = a;
  return state;
}

void rn_canvas_set_size(const struct rn_box_u32_t *canvas) { assert(canvas); }
