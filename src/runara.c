#include "../include/runara/runara.h"
#include "src/backend.h"
#include "src/internal.h"
#include "src/log.h"
#include <assert.h>
#include <stdlib.h>

#define RN_SUBSYS_NAME "CORE"

static void *_default_alloc(size_t size, void *ctx);
static void *_default_realloc(void *ptr, size_t size, void *ctx);
static void _default_free(void *ptr, void *ctx);
static void _log_init(struct rn_state_t *state,
                      const struct rn_log_options_t *log_opts);
static const char *_backend_type_to_str(enum rn_backend_type_t type);

static void *_default_alloc(size_t size, void *ctx) {
  (void)ctx;
  return malloc(size);
}

static void *_default_realloc(void *ptr, size_t size, void *ctx) {
  (void)ctx;
  return realloc(ptr, size);
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

const char *rn_result_to_str(rn_result_t result) {
  switch (result) {
  case RN_OK:
    return "Success";
  case RN_ERROR_INVALID_ARGUMENT:
    return "Invalid argument";
  case RN_ERROR_OUT_OF_MEMORY:
    return "Out of memory";
  case RN_ERROR_BACKEND:
    return "Rendering backend error";
  case RN_ERROR_BAD_STATE:
    return "Invalid renderer state";
  case RN_ERROR_OVERFLOW:
    return "Buffer or integer overflow";
  case RN_ERROR_ATLAS_FULL:
    return "Texture atlas is full";
  default:
    return "Unknown Runara error";
  }
}

struct rn_state_t *rn_state_init(const struct rn_init_options_t *options,
                                 uint32_t init_width, uint32_t init_height) {
  struct rn_init_options_t opts = {0};

  if (options)
    opts = *options;

  struct rn_allocator_t allocator = opts.allocator;

  if (!allocator.alloc && !allocator.free) {
    allocator.alloc = _default_alloc;
    allocator.free = _default_free;
    allocator.realloc = _default_realloc;
  } else if (!allocator.alloc || !allocator.free) {
    RN_ERROR_HEADLESS("allocator requires both alloc and free callbacks");
    return NULL;
  }

  if (!allocator.realloc)
    allocator.realloc = _default_realloc;

  struct rn_state_t *state = allocator.alloc(sizeof(*state), allocator.ctx);

  if (!state) {
    RN_ERROR_HEADLESS("failed to allocate Runara state");
    return NULL;
  }

  *state = (struct rn_state_t){0};
  state->allocator = allocator;

  RN_TRACE(state->log, "State allocated");

  _log_init(state, &opts.log);
  RN_TRACE(state->log, "Log initialized");

  if (!rn_backend_implement(&state->backend, opts.render_backend,
                            &state->allocator, &state->log,
                            &opts.gl_load_proc)) {
    rn_state_fini(state);

    RN_FATAL(state->log, "Failed to implement rendering backend (Backend: %s)",
             _backend_type_to_str(opts.render_backend));
    return NULL;
  }
  state->backend_init = true;
  state->backend_type = opts.render_backend;

  state->renderer.canvas.width = init_width;
  state->renderer.canvas.height = init_height;

  RN_TRACE(state->log, "Render backend initialized (Backend: %s)",
           _backend_type_to_str(opts.render_backend));

  int renderer_res = rn_renderer_init(state);
  if (renderer_res != RN_OK) {
    rn_state_fini(state);

    RN_FATAL(state->log, "Failed to initialize renderer: %s",
             rn_result_to_str(renderer_res));

    return NULL;
  }
  state->renderer_init = true;

  RN_INFO(state->log, "Runara initialized successfully");

  return state;
}

void rn_state_fini(struct rn_state_t *state) {
  if (!state) {
    RN_ERROR_HEADLESS("Trying to destroy NULL Runara state");
    return;
  }

  RN_TRACE(state->log, "destroying Runara state");

  if (state->renderer_init) {
    assert(state->backend_init);
    rn_renderer_fini(state);
  }

  if (state->backend_init)
    rn_backend_fini(&state->backend, state->backend_type, &state->allocator);

  if (state->owns_log_stream)
    fclose(state->log.stream);

  state->allocator.free(state, state->allocator.ctx);
}
