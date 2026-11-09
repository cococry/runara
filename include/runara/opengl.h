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

/* Tells runara that it owns the following GL state between frames:
 *   - VAO/buffer bindings
 *   - Current shader program binding
 *   - Shader uniform bindings
 *   - Viewport position and dimension
 *   - Vertex attributes
 *   - Blending
 *   - Depth testing
 *   - Face culling
 *
 * You may NOT change any of this state if you set preserved to true.
 *
 * When this is enabled, runara will skip redundant program/VAO/buffer bindings,
 * viewport setup and uniform updates and it will skip unchanged instance buffer
 * uploads. This improves performance.
 *
 * Calling this function invalidates every already cached render state once.
 */
rn_result_t rn_gl_set_state_preserved(struct rn_state_t *state, bool preserved);
