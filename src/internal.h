#pragma once

#include "runara/runara.h"
#include "src/backend.h"
#include "src/log.h"
#include "src/renderer.h"

struct rn_state_t {
  struct rn_allocator_t allocator;
  struct rn_log_t log;
  bool owns_log_stream;

  struct rn_backend_impl_t backend;
  bool backend_init;

  enum rn_backend_type_t backend_type;

  struct rn_renderer_t renderer;
  bool renderer_init;
};
