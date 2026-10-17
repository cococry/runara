#include "../backend.h"

bool rn_vk_backend_implement(struct rn_backend_impl_t *impl,
                             const struct rn_allocator_t *alloc,
                             const struct rn_log_t *log, void *backend_info) {
  (void)impl;
  (void)alloc;
  (void)log;
  (void)backend_info;

  return false;
}

void rn_vk_backend_fini(struct rn_backend_impl_t *impl,
                        const struct rn_allocator_t *alloc) {
  (void)impl;
  (void)alloc;
}
