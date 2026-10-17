#include "backend.h"
#include "src/log.h"

#define RN_SUBSYS_NAME "BACKEND"

bool rn_backend_implement(struct rn_backend_impl_t *impl,
                          enum rn_backend_type_t type) {
  if (!impl)
    return false;

  switch (type) {
  case RN_RENDER_BACKEND_OPENGL:
#if defined(RN_HAS_OPENGL) && RN_HAS_OPENGL
    return rn_gl_backend_implement(impl);
#else
    RN_ERROR_HEADLESS("OpenGL backend was not compiled into Runara");
    return false;
#endif

  case RN_RENDER_BACKEND_VULKAN:
#if defined(RN_HAS_VULKAN) && RN_HAS_VULKAN
    return rn_vk_backend_implement(impl);
#else
    RN_ERROR_HEADLESS("Vulkan backend was not compiled into Runara");
    return false;
#endif

  default:
    RN_ERROR_HEADLESS("Unknown Runara backend: %d", (int)type);
    return false;
  }
}
