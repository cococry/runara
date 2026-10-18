#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A color specified by 0.0-1.0 RGBA channels. */
typedef struct rn_color_t {
  float r, g, b, a;
} rn_color_t;

/* A box specified by 32-bit unsigned integer bounds. */
struct rn_box_u32_t {
  uint32_t x, y, width, height;
};

/* Opaque runara state object */
struct rn_state_t;

typedef void *(*rn_alloc_fn_t)(size_t size, void *ctx);
typedef void *(*rn_realloc_fn_t)(void *ptr, size_t size, void *ctx);
typedef void (*rn_free_fn_t)(void *ptr, void *ctx);

/* Specifies a custom allocator for Runara. All three callbacks must be
 * specified for a valid custom allocator. The ctx pointer can be used as
 * user data and is passed to all allocation functions. */
struct rn_allocator_t {
  rn_alloc_fn_t alloc;
  rn_realloc_fn_t realloc;
  rn_free_fn_t free;
  void *ctx;
};

/* Specifies the logging options for Runara */
struct rn_log_options_t {
  /* If Runara should log trace logs */
  bool verbose;
  /* If Runara should not log to the console (still logs to the logfile if
   * logfile is true)*/
  bool quiet;
  /* If Runara should log to a logfile (at
   * ~/.local/state/runara/logs/runara-[date-and-time]) */
  bool logfile;
};

/* Specifies a render backend to use with Runara */
enum rn_backend_type_t {
  /* Uses OpenGL 3.3 internally */
  RN_RENDER_BACKEND_OPENGL = 0,
  /* Uses Vulkan 1.0 internally */
  RN_RENDER_BACKEND_VULKAN
};

/* Specifies a callback to load OpenGL functions */
typedef void *(*rn_gl_load_proc)(const char *name);
struct rn_vk_init_options_t;

struct rn_init_options_t {
  /* optional, when not supplied, libc allocator functions are used (malloc(),
   * free(), realloc() )*/
  struct rn_allocator_t allocator;

  /* Options for logging */
  struct rn_log_options_t log;

  /* The render backend to use for this Runara state */
  enum rn_backend_type_t render_backend;

  /* optional, but needs to be supplied for OpenGL */
  const rn_gl_load_proc gl_load_proc;
  /* optional, but needs to be supplied for Vulkan */
  const struct rn_vk_init_options_t *vk_options;
};

struct rn_rect_t {
  float x, y, width, height;
};

/* Image storage */
enum rn_image_storage_t {
  /* Dedicated storage. Allocates a dedicated render texture for the image.*/
  RN_IMAGE_DEDICATED,
  /* Atlas storage. Allocates place in the internal 3D image atlas and stores
     the image in the atlas.*/
  RN_IMAGE_ATLAS
};

/* An opaque handle for a render buffer */
typedef uint32_t rn_buffer_handle_t;
/* An opaque handle for a render texture */
typedef uint32_t rn_texture_handle_t;
/* An opaque handle for a render pipeline */
typedef uint32_t rn_pipeline_handle_t;

/* Specifies data for an Image */
struct rn_image_t {
  enum rn_image_storage_t storage;
  rn_texture_handle_t texture;
  /* The width in pixels of the image*/
  uint32_t width;
  /* The height in pixels of the image*/
  uint32_t height;
  /* The layer (depth) in the 3D image atlas. Only used if storage is
   * RN_IMAGE_ATLAS */
  uint32_t layer;
  /* The uv coordinates of the image */
  float uv[4];
};

typedef enum rn_result_t {
  RN_OK = 0,
  RN_ERROR_INVALID_ARGUMENT,
  RN_ERROR_OUT_OF_MEMORY,
  RN_ERROR_BACKEND,
  RN_ERROR_BAD_STATE,
  RN_ERROR_OVERFLOW,
  RN_ERROR_ATLAS_FULL
} rn_result_t;

/* Get the human readable string from an rn_result_t. */
const char *rn_result_to_str(rn_result_t result);

/* Upload a dedicated texture to the renderer. Runara returns the created
 * rn_image_t image.*/
rn_result_t rn_texture_upload(struct rn_state_t *state, uint32_t width,
                              uint32_t height, const void *rgba,
                              size_t byte_count, struct rn_image_t *image);

/* Upload an atlas txture to the renderer. Runara returns the created rn_image_t
 * image.*/
rn_result_t rn_atlas_upload(struct rn_state_t *state, uint32_t width,
                            uint32_t height, const void *rgba,
                            size_t byte_count, struct rn_image_t *image);

/* Destroy a texture. */
void rn_texture_release(struct rn_state_t *state, struct rn_image_t *image);

/* Begin drawing with Runara. */
rn_result_t rn_begin(struct rn_state_t *state);

/* Clear color buffer. */
rn_result_t rn_clear_color(struct rn_state_t *state, struct rn_color_t color);

/* Draw a rectangle. */
rn_result_t rn_draw_rect(struct rn_state_t *state, struct rn_rect_t rect,
                         struct rn_color_t color);

/* Draw a texture from an rn_image_t with a tint at a given rect. */
rn_result_t rn_draw_texture(struct rn_state_t *state,
                            const struct rn_image_t *image,
                            struct rn_rect_t rect, struct rn_color_t tint);

/* End drawing with Runara. */
rn_result_t rn_end(struct rn_state_t *state);

/* Init runara state. init_width and init_height are used as the initial
 * canvas dimensions. */
struct rn_state_t *rn_state_init(const struct rn_init_options_t *options,
                                 uint32_t init_width, uint32_t init_height);

/* Destroy runara state. */
void rn_state_fini(struct rn_state_t *state);

/* Set canvas bounds. */
void rn_canvas_set_bounds(struct rn_state_t *state,
                          const struct rn_box_u32_t *canvas);
