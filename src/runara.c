#include "../include/runara/runara.h"
#include "src/backend.h"
#include "src/log.h"
#include <assert.h>
#include <stdlib.h>

#define RN_SUBSYS_NAME "CORE"

struct rn_state_t {
  struct rn_allocator_t allocator;
  struct rn_log_t log;
  bool owns_log_stream;

  struct rn_backend_impl_t impl;
  enum rn_backend_type_t backend_type;
};

static void *_default_alloc(size_t size, void *ctx);
static void _default_free(void *ptr, void *ctx);
static void _log_init(struct rn_state_t *state,
                      const struct rn_log_options_t *log_opts);
static const char *_backend_type_to_str(enum rn_backend_type_t type);

static void *_default_alloc(size_t size, void *ctx) {
  (void)ctx;
  return malloc(size);
}

static void _default_free(void *ptr, void *ctx) {
  (void)ctx;
  free(ptr);
}

static void _log_init(struct rn_state_t *state,
                      const struct rn_log_options_t *log_opts) {
  assert(state && log_opts);

  state->log = (struct rn_log_t){
      .stream = stdout,
      .verbose = log_opts->verbose,
      .quiet = log_opts->quiet,
  };

  if (log_opts->logfile) {
    const char *path = rn_log_get_filepath();
    FILE *stream = path ? fopen(path, "a") : NULL;

    if (stream) {
      RN_INFO(state->log, "Logging to logfile '%s'", path);
      setvbuf(stream, NULL, _IONBF, 0);

      state->log.stream = stream;
      state->owns_log_stream = true;
    } else {
      /* Fall back to stdout. */
      RN_ERROR_HEADLESS("failed to open Runara logfile");
    }
  }
}

static const char *_backend_type_to_str(enum rn_backend_type_t type) {
  switch (type) {
  case RN_RENDER_BACKEND_OPENGL:
    return "OpenGL";
  case RN_RENDER_BACKEND_VULKAN:
    return "Vulkan";
  }
  return "Unknown";
}

struct rn_state_t *rn_state_init(const struct rn_init_options_t *options) {
  struct rn_init_options_t opts = {0};

  if (options)
    opts = *options;

  struct rn_allocator_t allocator = opts.allocator;

  if (!allocator.alloc && !allocator.free) {
    allocator.alloc = _default_alloc;
    allocator.free = _default_free;
  } else if (!allocator.alloc || !allocator.free) {
    RN_ERROR_HEADLESS("allocator requires both alloc and free callbacks");
    return NULL;
  }

  struct rn_state_t *state = allocator.alloc(sizeof(*state), allocator.ctx);

  if (!state) {
    RN_ERROR_HEADLESS("failed to allocate Runara state");
    return NULL;
  }

  RN_TRACE(state->log, "State allocated");

  *state = (struct rn_state_t){0};
  state->allocator = allocator;

  _log_init(state, &opts.log);
  RN_TRACE(state->log, "Log initialized");

  if (!rn_backend_implement(&state->impl, opts.render_backend)) {
    RN_FATAL(state->log, "Failed to implement rendering backend (Backend: %s)",
             _backend_type_to_str(opts.render_backend));
    return NULL;
  }
  state->backend_type = opts.render_backend;

  RN_TRACE(state->log, "Render backend initialized (Backend: %s)",
           _backend_type_to_str(opts.render_backend));

  RN_INFO(state->log, "Runara initialized successfully");

  return state;
}

void rn_state_fini(struct rn_state_t *state) {
  if (!state) {
    RN_ERROR_HEADLESS("Trying to destroy NULL Runara state");
    return;
  }

  RN_TRACE(state->log, "destroying Runara state");

  if (state->owns_log_stream)
    fclose(state->log.stream);

  state->allocator.free(state, state->allocator.ctx);
}

void rn_canvas_set_size(const struct rn_box_u32_t *canvas) { assert(canvas); }
