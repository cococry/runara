/*
 * Copyright (c) 2026 Luca Machiedo
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

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
