#include "backend.h"
#include "src/log.h"

#define RN_SUBSYS_NAME "BACKEND"

bool rn_backend_implement(struct rn_backend_impl_t *impl,
                          enum rn_backend_type_t type,
                          const struct rn_allocator_t *alloc,
                          const struct rn_log_t *log, const void *backend_info) {

  if (!impl || !alloc)
    return false;

  switch (type) {
  case RN_RENDER_BACKEND_OPENGL:
#if defined(RN_HAS_OPENGL) && RN_HAS_OPENGL
    return rn_gl_backend_implement(impl, alloc, log, backend_info);
#else
    RN_ERROR_HEADLESS("OpenGL backend was not compiled into Runara");
    return false;
#endif

  case RN_RENDER_BACKEND_VULKAN:
#if defined(RN_HAS_VULKAN) && RN_HAS_VULKAN
    return rn_vk_backend_implement(impl, alloc, log, backend_info);
#else
    RN_ERROR_HEADLESS("Vulkan backend was not compiled into Runara");
    return false;
#endif

  default:
    RN_ERROR_HEADLESS("Unknown Runara backend: %d", (int)type);
    return false;
  }
}

void rn_backend_fini(struct rn_backend_impl_t *impl,
                     enum rn_backend_type_t type,
                     const struct rn_allocator_t *alloc) {
  if (!impl || !alloc)
    return;

  switch (type) {
  case RN_RENDER_BACKEND_OPENGL:
#if defined(RN_HAS_OPENGL) && RN_HAS_OPENGL
    rn_gl_backend_fini(impl, alloc);
    return;
#else
    RN_ERROR_HEADLESS("OpenGL backend was not compiled into Runara");
    return;
#endif

  case RN_RENDER_BACKEND_VULKAN:
#if defined(RN_HAS_VULKAN) && RN_HAS_VULKAN
    rn_vk_backend_fini(impl, alloc);
    return;
#else
    RN_ERROR_HEADLESS("Vulkan backend was not compiled into Runara");
    return;
#endif

  default:
    RN_ERROR_HEADLESS("Unknown Runara backend: %d", (int)type);
    return;
  }
}
