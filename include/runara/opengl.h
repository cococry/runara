#pragma once

#include "runara/runara.h"

/* Borrow an existing OpenGL 2D texture.
 * Runara registers the texture for rendering but does not own it.
 * The caller must ensure the texture belongs to the current OpenGL context
 * or a compatible shared context.
 *
 * The texture must remain valid until rn_texture_release() is called.
 * rn_texture_release() destroys the Runara wrapper, not the GL texture.
 */
rn_result_t rn_image_from_gl_texture(struct rn_state_t *state, uint32_t texture,
                                     uint32_t width, uint32_t height,
                                     struct rn_image_t *out_image);
