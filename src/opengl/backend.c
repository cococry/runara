#include "src/backend.h"
#include "runara/runara.h"
#include "src/log.h"
#include <glad/glad.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RN_GL_MAX_BINDINGS 8

#define RN_SUBSYS_NAME "OPENGL"

#define _RN_GL_LOG_ERROR(log, ...)                                             \
  do {                                                                         \
    if ((log) != NULL)                                                         \
      RN_ERROR(*(log), __VA_ARGS__);                                           \
    else                                                                       \
      RN_ERROR_HEADLESS(__VA_ARGS__);                                          \
  } while (0)

#define _RN_GL_LOG_TRACE(log, ...)                                             \
  do {                                                                         \
    if ((log) != NULL)                                                         \
      RN_TRACE(*(log), __VA_ARGS__);                                           \
  } while (0)

struct rn_gl_buffer {
  GLuint gl_id;
  GLenum target;
  size_t size;
  bool alive;
};

struct rn_gl_texture {
  GLuint gl_id;
  GLenum target;
  uint32_t width, height, layers;
  bool alive;
};

struct rn_gl_pipeline {
  GLuint gl_id;
  struct rn_vertex_buffer_layout_t layouts[RN_GL_MAX_BINDINGS];
  struct rn_vertex_attribute_t *attributes[RN_GL_MAX_BINDINGS];
  size_t layout_count;
  enum rn_blend_mode_t blend;
  bool depth_test;
  bool alive;
};

struct rn_gl_binding {
  rn_buffer_handle_t handle;
  size_t instance_base;
};

struct rn_gl_context {
  GLuint vertex_array;
  struct rn_gl_buffer *buffers;
  size_t buffer_count;
  struct rn_gl_texture *textures;
  size_t texture_count;
  struct rn_gl_pipeline *pipelines;
  size_t pipeline_count;
  rn_pipeline_handle_t pipeline;
  struct rn_gl_binding bindings[RN_GL_MAX_BINDINGS];
  rn_buffer_handle_t index_buffer;

  const struct rn_allocator_t *alloc;
  const struct rn_log_t *log;
};

static const char *_gl_error_name(GLenum error);
static rn_result_t _gl_check(const struct rn_log_t *log, const char *operation);
static bool _handle_count_fits(size_t count);
static bool _gl_size_fits(size_t size);
static struct rn_gl_buffer *_buffer(struct rn_gl_context *ctx,
                                    rn_buffer_handle_t handle);
static struct rn_gl_texture *_texture(struct rn_gl_context *ctx,
                                      rn_texture_handle_t handle);
static struct rn_gl_pipeline *_pipeline(struct rn_gl_context *ctx,
                                        rn_pipeline_handle_t handle);
static rn_result_t _ensure_buffers(struct rn_gl_context *ctx);
static rn_result_t _ensure_textures(struct rn_gl_context *ctx);
static rn_result_t _ensure_pipelines(struct rn_gl_context *ctx);
static rn_result_t _begin_frame(void *backend_ctx, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height);
static rn_result_t _clear_color(void *ctx, rn_color_t color);
static rn_result_t _end_frame(void *backend_ctx);
static rn_result_t _create_buffer(void *backend_ctx,
                                  const struct rn_buffer_desc_t *desc,
                                  rn_buffer_handle_t *out_handle);
static void _destroy_buffer(void *backend_ctx, rn_buffer_handle_t handle);
static rn_result_t _write_buffer(void *backend_ctx, rn_buffer_handle_t handle,
                                 uint32_t binding, size_t offset,
                                 const void *data, size_t size);
static GLuint _compile_shader(struct rn_gl_context *ctx, GLenum shader_type,
                              const char *source);
static GLuint _create_shader_program(struct rn_gl_context *ctx,
                                     enum rn_shader_pipeline_t shader_pipeline);
static rn_result_t _create_pipeline(void *backend_ctx,
                                    const struct rn_pipeline_desc_t *desc,
                                    rn_pipeline_handle_t *out_handle);
static void _destroy_pipeline(void *backend_ctx, rn_pipeline_handle_t handle);
static rn_result_t _set_pipeline(void *backend_ctx,
                                 rn_pipeline_handle_t handle);
static rn_result_t _apply_binding(struct rn_gl_context *ctx, uint32_t binding,
                                  size_t first_instance);
static rn_result_t _set_vertex_buffer(void *backend_ctx,
                                      rn_buffer_handle_t handle,
                                      uint32_t binding);
static rn_result_t _set_index_buffer(void *backend_ctx,
                                     rn_buffer_handle_t handle);
static rn_result_t _set_uniform_2f(void *backend_ctx,
                                   rn_pipeline_handle_t handle,
                                   const char *name, float value_x,
                                   float value_y);
static rn_result_t
_draw_indexed_instanced(void *backend_ctx,
                        const struct rn_draw_indexed_instanced_t *draw);
static rn_result_t _create_texture_internal(void *backend_ctx, GLenum target,
                                            uint32_t width, uint32_t height,
                                            uint32_t layers,
                                            const void *rgba_pixels,
                                            size_t data_size,
                                            rn_texture_handle_t *out_handle);
static rn_result_t _create_texture(void *backend_ctx,
                                   const struct rn_texture_desc_t *desc,
                                   const void *rgba_pixels, size_t size,
                                   rn_texture_handle_t *out_handle);
static rn_result_t
_create_texture_array(void *backend_ctx,
                      const struct rn_texture_array_desc_t *desc,
                      rn_texture_handle_t *out_handle);
static rn_result_t
_write_texture_array_layer(void *backend_ctx,
                           const struct rn_texture_array_write_t *write,
                           const void *rgba_pixels, size_t size);
static void _destroy_texture(void *backend_ctx, rn_texture_handle_t handle);
static rn_result_t _set_texture(void *backend_ctx, uint32_t slot,
                                rn_texture_handle_t handle);

static const struct rn_backend_impl_t _rn_gl_dispatch = {
    .begin_frame = _begin_frame,
    .clear_color = _clear_color,
    .end_frame = _end_frame,
    .create_buffer = _create_buffer,
    .destroy_buffer = _destroy_buffer,
    .write_buffer = _write_buffer,
    .create_pipeline = _create_pipeline,
    .destroy_pipeline = _destroy_pipeline,
    .set_pipeline = _set_pipeline,
    .set_vertex_buffer = _set_vertex_buffer,
    .set_index_buffer = _set_index_buffer,
    .set_uniform_2f = _set_uniform_2f,
    .draw_indexed_instanced = _draw_indexed_instanced,
    .create_texture = _create_texture,
    .create_texture_array = _create_texture_array,
    .write_texture_array_layer = _write_texture_array_layer,
    .destroy_texture = _destroy_texture,
    .set_texture = _set_texture,
};

/* Shaders shared by the atlas and dedicated-texture pipelines. */
static const char *_vertex_src =
    "#version 330 core\n"
    "\n"
    "layout(location = 0) in vec2 a_local_pos;\n"
    "layout(location = 1) in vec2 a_local_uv;\n"
    "layout(location = 2) in vec4 i_rect;\n"
    "layout(location = 3) in vec4 i_color;\n"
    "layout(location = 4) in vec4 i_uv;\n"
    "layout(location = 5) in vec4 i_params;\n"
    "\n"
    "uniform vec2 u_screen_size;\n"
    "\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "flat out int v_layer;\n"
    "flat out int v_kind;\n"
    "\n"
    "void main() {\n"
    "  vec2 pixel_pos = i_rect.xy + a_local_pos * i_rect.zw;\n"
    "  vec2 ndc = vec2(\n"
    "      pixel_pos.x / u_screen_size.x * 2.0 - 1.0,\n"
    "      1.0 - pixel_pos.y / u_screen_size.y * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = mix(i_uv.xy, i_uv.zw, a_local_uv);\n"
    "  v_color = i_color;\n"
    "  v_layer = int(i_params.z + 0.5);\n"
    "  v_kind = int(i_params.w + 0.5);\n"
    "}\n";

static const char *_frag_atlas =
    "#version 330 core\n"
    "\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "flat in int v_layer;\n"
    "flat in int v_kind;\n"
    "uniform sampler2DArray u_texture_array;\n"
    "out vec4 out_color;\n"
    "\n"
    "void main() {\n"
    "  if (v_kind == 0) {\n"
    "    out_color = v_color;\n"
    "    return;\n"
    "  }\n"
    "  vec4 texel = texture(u_texture_array,\n"
    "                       vec3(v_uv, float(v_layer)));\n"
    "  if (v_kind == 1) {\n"
    "    out_color = vec4(v_color.rgb, v_color.a * texel.a);\n"
    "  } else {\n"
    "    out_color = texel * v_color;\n"
    "  }\n"
    "}\n";

static const char *_frag_dedicated =
    "#version 330 core\n"
    "\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "uniform sampler2D u_texture;\n"
    "out vec4 out_color;\n"
    "\n"
    "void main() {\n"
    "  out_color = texture(u_texture, v_uv) * v_color;\n"
    "}\n";

static bool glad_loaded = false;

static const char *_gl_error_name(GLenum error) {
  switch (error) {
  case GL_INVALID_ENUM:
    return "GL_INVALID_ENUM";
  case GL_INVALID_VALUE:
    return "GL_INVALID_VALUE";
  case GL_INVALID_OPERATION:
    return "GL_INVALID_OPERATION";
  case GL_OUT_OF_MEMORY:
    return "GL_OUT_OF_MEMORY";
  case GL_INVALID_FRAMEBUFFER_OPERATION:
    return "GL_INVALID_FRAMEBUFFER_OPERATION";
  default:
    return "UNKNOWN_GL_ERROR";
  }
}

static rn_result_t _gl_check(const struct rn_log_t *log,
                             const char *operation) {
  rn_result_t result = RN_OK;
  GLenum error = GL_NO_ERROR;

  while ((error = glGetError()) != GL_NO_ERROR) {
    _RN_GL_LOG_ERROR(log, "OpenGL %s: %s (0x%04x)", operation,
                     _gl_error_name(error), (unsigned int)error);

    result = RN_ERROR_BACKEND;
  }

  return result;
}

static bool _handle_count_fits(size_t count) { return count < UINT32_MAX; }

static bool _gl_size_fits(size_t size) { return size <= (size_t)PTRDIFF_MAX; }

static struct rn_gl_buffer *_buffer(struct rn_gl_context *ctx,
                                    rn_buffer_handle_t handle) {
  return handle < ctx->buffer_count && ctx->buffers[handle].alive
             ? &ctx->buffers[handle]
             : NULL;
}

static struct rn_gl_texture *_texture(struct rn_gl_context *ctx,
                                      rn_texture_handle_t handle) {
  return handle < ctx->texture_count && ctx->textures[handle].alive
             ? &ctx->textures[handle]
             : NULL;
}

static struct rn_gl_pipeline *_pipeline(struct rn_gl_context *ctx,
                                        rn_pipeline_handle_t handle) {
  return handle < ctx->pipeline_count && ctx->pipelines[handle].alive
             ? &ctx->pipelines[handle]
             : NULL;
}

static rn_result_t _ensure_buffers(struct rn_gl_context *ctx) {
  if (!_handle_count_fits(ctx->buffer_count) ||
      ctx->buffer_count == SIZE_MAX / sizeof(*ctx->buffers))
    return RN_ERROR_OVERFLOW;

  void *new_buffers = ctx->alloc->realloc(
      ctx->buffers, (ctx->buffer_count + 1) * sizeof(*ctx->buffers),
      ctx->alloc->ctx);

  if (!new_buffers)
    return RN_ERROR_OUT_OF_MEMORY;

  ctx->buffers = new_buffers;
  return RN_OK;
}

static rn_result_t _ensure_textures(struct rn_gl_context *ctx) {
  if (!_handle_count_fits(ctx->texture_count) ||
      ctx->texture_count == SIZE_MAX / sizeof(*ctx->textures))
    return RN_ERROR_OVERFLOW;

  void *new_textures = ctx->alloc->realloc(
      ctx->textures, (ctx->texture_count + 1) * sizeof(*ctx->textures),
      ctx->alloc->ctx);

  if (!new_textures)
    return RN_ERROR_OUT_OF_MEMORY;

  ctx->textures = new_textures;
  return RN_OK;
}

static rn_result_t _ensure_pipelines(struct rn_gl_context *ctx) {
  if (!_handle_count_fits(ctx->pipeline_count) ||
      ctx->pipeline_count == SIZE_MAX / sizeof(*ctx->pipelines))
    return RN_ERROR_OVERFLOW;

  void *new_pipelines = ctx->alloc->realloc(
      ctx->pipelines, (ctx->pipeline_count + 1) * sizeof(*ctx->pipelines),
      ctx->alloc->ctx);

  if (!new_pipelines)
    return RN_ERROR_OUT_OF_MEMORY;

  ctx->pipelines = new_pipelines;
  return RN_OK;
}

static rn_result_t _begin_frame(void *backend_ctx, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx || width > INT_MAX || height > INT_MAX)
    return RN_ERROR_INVALID_ARGUMENT;

  glViewport((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);
  glBindVertexArray(ctx->vertex_array);
  return _gl_check(ctx->log, __func__);
}

static rn_result_t _clear_color(void *backend_ctx, rn_color_t color) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx)
    return RN_ERROR_INVALID_ARGUMENT;

  glClearColor(color.r, color.g, color.b, color.a);
  glClear(GL_COLOR_BUFFER_BIT);

  return _gl_check(ctx->log, __func__);
}

static rn_result_t _end_frame(void *backend_ctx) {
  return backend_ctx
             ? _gl_check(((struct rn_gl_context *)backend_ctx)->log, __func__)
             : RN_ERROR_INVALID_ARGUMENT;
}

static rn_result_t _create_buffer(void *backend_ctx,
                                  const struct rn_buffer_desc_t *desc,
                                  rn_buffer_handle_t *out_handle) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx || !desc || !out_handle || !_gl_size_fits(desc->size))
    return RN_ERROR_INVALID_ARGUMENT;

  if (desc->target != RN_BUFFER_VERTEX && desc->target != RN_BUFFER_INDEX)
    return RN_ERROR_INVALID_ARGUMENT;

  GLenum usage =
      desc->usage == RN_BUFFER_STATIC ? GL_STATIC_DRAW
      : desc->usage == RN_BUFFER_DYNAMIC || desc->usage == RN_BUFFER_STAGING
          ? GL_DYNAMIC_DRAW
          : 0;
  if (!usage)
    return RN_ERROR_INVALID_ARGUMENT;

  rn_result_t result = _ensure_buffers(ctx);

  if (result)
    return result;
  GLuint gl_id = 0;
  GLenum target = desc->target == RN_BUFFER_INDEX ? GL_ELEMENT_ARRAY_BUFFER
                                                  : GL_ARRAY_BUFFER;
  glGenBuffers(1, &gl_id);
  if (!gl_id) {
    _RN_GL_LOG_ERROR(ctx->log, "glGenBuffers failed");
    _gl_check(ctx->log, __func__);
    return RN_ERROR_BACKEND;
  }

  glBindBuffer(target, gl_id);
  glBufferData(target, (GLsizeiptr)desc->size, desc->data, usage);
  if (_gl_check(ctx->log, __func__)) {
    glDeleteBuffers(1, &gl_id);
    return RN_ERROR_BACKEND;
  }
  *out_handle = (rn_buffer_handle_t)ctx->buffer_count;
  ctx->buffers[ctx->buffer_count++] =
      (struct rn_gl_buffer){gl_id, target, desc->size, true};

  _RN_GL_LOG_TRACE(ctx->log, "created buffer %u (GL=%u, size=%zu bytes)",
                   (unsigned int)*out_handle, (unsigned int)gl_id, desc->size);

  return RN_OK;
}

static void _destroy_buffer(void *backend_ctx, rn_buffer_handle_t handle) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx)
    return;

  struct rn_gl_buffer *gpu_buffer = _buffer(ctx, handle);

  if (!gpu_buffer)
    return;

  _RN_GL_LOG_TRACE(ctx->log, "destroying buffer %u (GL=%u)",
                   (unsigned int)handle, (unsigned int)gpu_buffer->gl_id);

  glDeleteBuffers(1, &gpu_buffer->gl_id);
  gpu_buffer->alive = false;
  for (size_t binding_index = 0; binding_index < RN_GL_MAX_BINDINGS;
       ++binding_index)
    if (ctx->bindings[binding_index].handle == handle)
      ctx->bindings[binding_index].handle = RN_INVALID_HANDLE;
  if (ctx->index_buffer == handle)
    ctx->index_buffer = RN_INVALID_HANDLE;
}

static rn_result_t _write_buffer(void *backend_ctx, rn_buffer_handle_t handle,
                                 uint32_t binding, size_t offset,
                                 const void *data, size_t size) {
  (void)binding;
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx || (!data && size))
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_gl_buffer *gpu_buffer = _buffer(ctx, handle);

  if (!gpu_buffer || offset > gpu_buffer->size ||
      size > gpu_buffer->size - offset || !_gl_size_fits(offset) ||
      !_gl_size_fits(size))
    return RN_ERROR_INVALID_ARGUMENT;

  glBindBuffer(gpu_buffer->target, gpu_buffer->gl_id);
  glBufferSubData(gpu_buffer->target, (GLintptr)offset, (GLsizeiptr)size, data);
  return _gl_check(ctx->log, __func__);
}

static GLuint _compile_shader(struct rn_gl_context *ctx, GLenum shader_type,
                              const char *source) {
  GLuint shader = glCreateShader(shader_type);

  if (!shader) {
    _RN_GL_LOG_ERROR(ctx->log, "glCreateShader failed (%s)",
                     shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment");
    _gl_check(ctx->log, __func__);
    return 0;
  }

  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint compiled = 0;

  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    GLchar info_log[2048];
    GLsizei log_length = 0;

    glGetShaderInfoLog(shader, (GLsizei)sizeof(info_log), &log_length,
                       info_log);
    _RN_GL_LOG_ERROR(ctx->log, "OpenGL %s shader compilation failed: %.*s",
                     shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment",
                     (int)log_length, info_log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint
_create_shader_program(struct rn_gl_context *ctx,
                       enum rn_shader_pipeline_t shader_pipeline) {
  GLuint vertex_shader = _compile_shader(ctx, GL_VERTEX_SHADER, _vertex_src);

  if (!vertex_shader)
    return 0;

  GLuint fragment_shader = _compile_shader(
      ctx, GL_FRAGMENT_SHADER,
      shader_pipeline == RN_PIPELINE_UI_QUAD_ATLAS ? _frag_atlas
                                                   : _frag_dedicated);
  if (!fragment_shader) {
    glDeleteShader(vertex_shader);
    return 0;
  }

  GLuint program = glCreateProgram();

  if (!program) {
    _RN_GL_LOG_ERROR(ctx->log, "glCreateProgram failed");
    _gl_check(ctx->log, __func__);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    return 0;
  }

  glAttachShader(program, vertex_shader);
  glAttachShader(program, fragment_shader);
  glLinkProgram(program);
  glDetachShader(program, vertex_shader);
  glDetachShader(program, fragment_shader);
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
  GLint linked = 0;

  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    GLchar info_log[2048];
    GLsizei log_length = 0;

    glGetProgramInfoLog(program, (GLsizei)sizeof(info_log), &log_length,
                        info_log);
    _RN_GL_LOG_ERROR(ctx->log, "OpenGL shader program linking failed: %.*s",
                     (int)log_length, info_log);
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

static rn_result_t _create_pipeline(void *backend_ctx,
                                    const struct rn_pipeline_desc_t *desc,
                                    rn_pipeline_handle_t *out_handle) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx || !desc || !out_handle ||
      desc->vertex_binding_count > RN_GL_MAX_BINDINGS ||
      (desc->vertex_binding_count && !desc->vertex_bindings) ||
      (desc->uniform_binding_count && !desc->uniform_bindings) ||
      (desc->shader != RN_PIPELINE_UI_QUAD_ATLAS &&
       desc->shader != RN_PIPELINE_UI_QUAD_DEDICATED) ||
      desc->blend > RN_BLEND_PREMULTIPLIED_ALPHA)
    return RN_ERROR_INVALID_ARGUMENT;

  for (size_t binding_index = 0; binding_index < desc->vertex_binding_count;
       ++binding_index) {
    const struct rn_vertex_buffer_layout_t *layout =
        &desc->vertex_bindings[binding_index];

    if (layout->binding >= RN_GL_MAX_BINDINGS ||
        (layout->stride == 0 || layout->stride > INT_MAX) ||
        (layout->attribute_count && !layout->attributes) ||
        layout->attribute_count >
            SIZE_MAX / sizeof(struct rn_vertex_attribute_t))
      return RN_ERROR_INVALID_ARGUMENT;

    for (size_t attribute_index = 0; attribute_index < layout->attribute_count;
         ++attribute_index)
      if (layout->attributes[attribute_index].location >= 16 ||
          layout->attributes[attribute_index].format > RN_VERTEX_FLOAT32X4)
        return RN_ERROR_INVALID_ARGUMENT;
  }

  rn_result_t result = _ensure_pipelines(ctx);

  if (result)
    return result;
  struct rn_gl_pipeline gpu_pipeline = {0};

  gpu_pipeline.gl_id = _create_shader_program(ctx, desc->shader);
  if (!gpu_pipeline.gl_id)
    return RN_ERROR_BACKEND;

  gpu_pipeline.blend = desc->blend;
  gpu_pipeline.depth_test = desc->depth_test;
  gpu_pipeline.layout_count = desc->vertex_binding_count;
  gpu_pipeline.alive = true;

  for (size_t binding_index = 0; binding_index < gpu_pipeline.layout_count;
       ++binding_index) {
    const struct rn_vertex_buffer_layout_t *layout =
        &desc->vertex_bindings[binding_index];
    gpu_pipeline.layouts[binding_index] = *layout;

    if (layout->attribute_count) {
      gpu_pipeline.attributes[binding_index] =
          ctx->alloc->alloc(layout->attribute_count *
                                sizeof(*gpu_pipeline.attributes[binding_index]),
                            ctx->alloc->ctx);

      if (!gpu_pipeline.attributes[binding_index]) {
        result = RN_ERROR_OUT_OF_MEMORY;
        goto fail;
      }

      memcpy(gpu_pipeline.attributes[binding_index], layout->attributes,
             layout->attribute_count *
                 sizeof(*gpu_pipeline.attributes[binding_index]));
      gpu_pipeline.layouts[binding_index].attributes =
          gpu_pipeline.attributes[binding_index];
    }
  }

  glUseProgram(gpu_pipeline.gl_id);
  for (size_t binding_index = 0; binding_index < desc->uniform_binding_count;
       ++binding_index) {
    const struct rn_uniform_binding_t *uniform =
        &desc->uniform_bindings[binding_index];

    if (!uniform->name) {
      result = RN_ERROR_INVALID_ARGUMENT;
      goto fail;
    }

    GLint location = glGetUniformLocation(gpu_pipeline.gl_id, uniform->name);

    if (location < 0)
      continue;
    if (uniform->type == RN_UNIFORM_VEC2)
      glUniform2f(location, uniform->f_data[0], uniform->f_data[1]);
    else if (uniform->type == RN_UNIFORM_SAMPLER_2D ||
             uniform->type == RN_UNIFORM_SAMPLER_2D_ARRAY)
      glUniform1i(location, (GLint)uniform->binding);
    else {
      result = RN_ERROR_INVALID_ARGUMENT;
      goto fail;
    }
  }

  if (_gl_check(ctx->log, __func__)) {
    result = RN_ERROR_BACKEND;
    goto fail;
  }
  *out_handle = (rn_pipeline_handle_t)ctx->pipeline_count;
  ctx->pipelines[ctx->pipeline_count++] = gpu_pipeline;

  _RN_GL_LOG_TRACE(ctx->log, "created pipeline %u (GL program=%u)",
                   (unsigned int)*out_handle, (unsigned int)gpu_pipeline.gl_id);

  return RN_OK;

fail:
  for (size_t binding_index = 0; binding_index < gpu_pipeline.layout_count;
       ++binding_index)
    ctx->alloc->free(gpu_pipeline.attributes[binding_index], ctx->alloc->ctx);
  glDeleteProgram(gpu_pipeline.gl_id);
  return result;
}

static void _destroy_pipeline(void *backend_ctx, rn_pipeline_handle_t handle) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx)
    return;

  struct rn_gl_pipeline *gpu_pipeline = _pipeline(ctx, handle);

  if (!gpu_pipeline)
    return;

  _RN_GL_LOG_TRACE(ctx->log, "destroying pipeline %u (GL program=%u)",
                   (unsigned int)handle, (unsigned int)gpu_pipeline->gl_id);

  if (ctx->pipeline == handle)
    ctx->pipeline = RN_INVALID_HANDLE;

  glDeleteProgram(gpu_pipeline->gl_id);
  for (size_t binding_index = 0; binding_index < gpu_pipeline->layout_count;
       ++binding_index)
    ctx->alloc->free(gpu_pipeline->attributes[binding_index], ctx->alloc->ctx);
  gpu_pipeline->alive = false;
}

static rn_result_t _set_pipeline(void *backend_ctx,
                                 rn_pipeline_handle_t handle) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_gl_pipeline *gpu_pipeline = _pipeline(ctx, handle);

  if (!gpu_pipeline)
    return RN_ERROR_INVALID_ARGUMENT;

  glUseProgram(gpu_pipeline->gl_id);
  glBindVertexArray(ctx->vertex_array);
  if (gpu_pipeline->depth_test)
    glEnable(GL_DEPTH_TEST);
  else
    glDisable(GL_DEPTH_TEST);
  if (gpu_pipeline->blend == RN_BLEND_OPAQUE)
    glDisable(GL_BLEND);
  else {
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(gpu_pipeline->blend == RN_BLEND_PREMULTIPLIED_ALPHA
                    ? GL_ONE
                    : GL_SRC_ALPHA,
                GL_ONE_MINUS_SRC_ALPHA);
  }

  glDisable(GL_CULL_FACE);
  ctx->pipeline = handle;
  for (size_t binding_index = 0; binding_index < RN_GL_MAX_BINDINGS;
       ++binding_index)
    ctx->bindings[binding_index].handle = RN_INVALID_HANDLE;
  ctx->index_buffer = RN_INVALID_HANDLE;
  return _gl_check(ctx->log, __func__);
}

static rn_result_t _apply_binding(struct rn_gl_context *ctx, uint32_t binding,
                                  size_t first_instance) {
  struct rn_gl_pipeline *gpu_pipeline = _pipeline(ctx, ctx->pipeline);

  if (!gpu_pipeline || binding >= RN_GL_MAX_BINDINGS)
    return RN_ERROR_BAD_STATE;

  struct rn_gl_buffer *gpu_buffer = _buffer(ctx, ctx->bindings[binding].handle);

  if (!gpu_buffer || gpu_buffer->target != GL_ARRAY_BUFFER)
    return RN_ERROR_BAD_STATE;

  const struct rn_vertex_buffer_layout_t *layout = NULL;

  for (size_t binding_index = 0; binding_index < gpu_pipeline->layout_count;
       ++binding_index)
    if (gpu_pipeline->layouts[binding_index].binding == binding) {
      layout = &gpu_pipeline->layouts[binding_index];
      break;
    }

  if (!layout)
    return RN_ERROR_INVALID_ARGUMENT;

  glBindBuffer(GL_ARRAY_BUFFER, gpu_buffer->gl_id);
  for (size_t binding_index = 0; binding_index < layout->attribute_count;
       ++binding_index) {
    const struct rn_vertex_attribute_t *attribute =
        &layout->attributes[binding_index];
    size_t attribute_size = attribute->format == RN_VERTEX_FLOAT32X2 ? 8 : 16;

    if (attribute->offset > layout->stride ||
        attribute_size > layout->stride - attribute->offset ||
        first_instance > (SIZE_MAX - attribute->offset) / layout->stride)
      return RN_ERROR_OVERFLOW;

    size_t byte_offset = first_instance * layout->stride + attribute->offset;

    if (byte_offset > (size_t)PTRDIFF_MAX)
      return RN_ERROR_OVERFLOW;

    glEnableVertexAttribArray(attribute->location);
    glVertexAttribPointer(attribute->location,
                          attribute->format == RN_VERTEX_FLOAT32X2 ? 2 : 4,
                          GL_FLOAT, GL_FALSE, (GLsizei)layout->stride,
                          (const void *)(uintptr_t)byte_offset);
    glVertexAttribDivisor(attribute->location,
                          layout->step_mode == RN_STEP_INSTANCE ? 1 : 0);
  }

  ctx->bindings[binding].instance_base = first_instance;
  return _gl_check(ctx->log, __func__);
}

static rn_result_t _set_vertex_buffer(void *backend_ctx,
                                      rn_buffer_handle_t handle,
                                      uint32_t binding) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx || binding >= RN_GL_MAX_BINDINGS)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_gl_buffer *gpu_buffer = _buffer(ctx, handle);

  if (!gpu_buffer || gpu_buffer->target != GL_ARRAY_BUFFER)
    return RN_ERROR_INVALID_ARGUMENT;

  ctx->bindings[binding].handle = handle;
  return _apply_binding(ctx, binding, 0);
}

static rn_result_t _set_index_buffer(void *backend_ctx,
                                     rn_buffer_handle_t handle) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_gl_buffer *gpu_buffer = _buffer(ctx, handle);

  if (!gpu_buffer || gpu_buffer->target != GL_ELEMENT_ARRAY_BUFFER)
    return RN_ERROR_INVALID_ARGUMENT;

  glBindVertexArray(ctx->vertex_array);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu_buffer->gl_id);
  ctx->index_buffer = handle;
  return _gl_check(ctx->log, __func__);
}

static rn_result_t _set_uniform_2f(void *backend_ctx,
                                   rn_pipeline_handle_t handle,
                                   const char *name, float value_x,
                                   float value_y) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx || !name)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_gl_pipeline *gpu_pipeline = _pipeline(ctx, handle);

  if (!gpu_pipeline)
    return RN_ERROR_INVALID_ARGUMENT;

  glUseProgram(gpu_pipeline->gl_id);
  GLint location = glGetUniformLocation(gpu_pipeline->gl_id, name);

  if (location >= 0)
    glUniform2f(location, value_x, value_y);

  /* Uniform updates must not make GL state tracking silently incorrect. */
  if (ctx->pipeline != handle)
    ctx->pipeline = RN_INVALID_HANDLE;
  return _gl_check(ctx->log, __func__);
}

static rn_result_t
_draw_indexed_instanced(void *backend_ctx,
                        const struct rn_draw_indexed_instanced_t *draw) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx || !draw)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_gl_buffer *index_buffer = _buffer(ctx, ctx->index_buffer);
  struct rn_gl_pipeline *gpu_pipeline = _pipeline(ctx, ctx->pipeline);

  if (!index_buffer || !gpu_pipeline || draw->index_count > INT_MAX ||
      draw->instance_count > INT_MAX ||
      (uint64_t)draw->index_offset * 4u + (uint64_t)draw->index_count * 4u >
          index_buffer->size)
    return RN_ERROR_INVALID_ARGUMENT;

  for (size_t binding_index = 0; binding_index < gpu_pipeline->layout_count;
       ++binding_index) {
    const struct rn_vertex_buffer_layout_t *layout =
        &gpu_pipeline->layouts[binding_index];

    /* Skip any vertex-data buffers (RN_STEP_VERTEX) */
    if (layout->step_mode != RN_STEP_INSTANCE)
      continue;

    /* Makes GL read from instance at draw->instance_offset (simulates
     * glDrawElementsInstancedBaseInstance)*/
    rn_result_t result =
        _apply_binding(ctx, layout->binding, draw->instance_offset);

    if (result)
      return result;
  }

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer->gl_id);
  glDrawElementsInstancedBaseVertex(
      GL_TRIANGLES, (GLsizei)draw->index_count, GL_UNSIGNED_INT,
      (const void *)(uintptr_t)((size_t)draw->index_offset * 4u),
      (GLsizei)draw->instance_count, (GLint)draw->vertex_offset);
  return _gl_check(ctx->log, __func__);
}

static rn_result_t _create_texture_internal(void *backend_ctx, GLenum target,
                                            uint32_t width, uint32_t height,
                                            uint32_t layers,
                                            const void *rgba_pixels,
                                            size_t data_size,
                                            rn_texture_handle_t *out_handle) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx || !out_handle || !width || !height || !layers || width > INT_MAX ||
      height > INT_MAX || layers > INT_MAX)
    return RN_ERROR_INVALID_ARGUMENT;

  uint64_t required_bytes = (uint64_t)width * height;

  if (required_bytes > SIZE_MAX / 4u)
    return RN_ERROR_OVERFLOW;

  required_bytes *= 4u;
  if (rgba_pixels && data_size < (size_t)required_bytes)
    return RN_ERROR_INVALID_ARGUMENT;

  if (!rgba_pixels && data_size)
    return RN_ERROR_INVALID_ARGUMENT;

  rn_result_t result = _ensure_textures(ctx);

  if (result)
    return result;
  GLuint gl_id = 0;

  glGenTextures(1, &gl_id);
  if (!gl_id) {
    _RN_GL_LOG_ERROR(ctx->log, "glGenTextures failed");
    _gl_check(ctx->log, __func__);
    return RN_ERROR_BACKEND;
  }

  glBindTexture(target, gl_id);
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (target == GL_TEXTURE_2D)
    glTexImage2D(target, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba_pixels);
  else
    glTexImage3D(target, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height,
                 (GLsizei)layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  if (_gl_check(ctx->log, __func__)) {
    glDeleteTextures(1, &gl_id);
    return RN_ERROR_BACKEND;
  }
  *out_handle = (rn_texture_handle_t)ctx->texture_count;
  ctx->textures[ctx->texture_count++] =
      (struct rn_gl_texture){gl_id, target, width, height, layers, true};

  _RN_GL_LOG_TRACE(ctx->log, "created texture %u (GL=%u, %ux%u, layers=%u)",
                   (unsigned int)*out_handle, (unsigned int)gl_id, width,
                   height, layers);

  return RN_OK;
}

static rn_result_t _create_texture(void *backend_ctx,
                                   const struct rn_texture_desc_t *desc,
                                   const void *rgba_pixels, size_t size,
                                   rn_texture_handle_t *out_handle) {
  if (!desc || desc->format != RN_TEXTURE_RGBA8)
    return RN_ERROR_INVALID_ARGUMENT;

  return _create_texture_internal(backend_ctx, GL_TEXTURE_2D, desc->width,
                                  desc->height, 1, rgba_pixels, size,
                                  out_handle);
}

static rn_result_t
_create_texture_array(void *backend_ctx,
                      const struct rn_texture_array_desc_t *desc,
                      rn_texture_handle_t *out_handle) {
  if (!desc || desc->format != RN_TEXTURE_RGBA8)
    return RN_ERROR_INVALID_ARGUMENT;

  return _create_texture_internal(backend_ctx, GL_TEXTURE_2D_ARRAY, desc->width,
                                  desc->height, desc->layers, NULL, 0,
                                  out_handle);
}

static rn_result_t
_write_texture_array_layer(void *backend_ctx,
                           const struct rn_texture_array_write_t *write,
                           const void *rgba_pixels, size_t size) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx || !write || !rgba_pixels)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_gl_texture *gpu_texture = _texture(ctx, write->texture);

  if (!gpu_texture || gpu_texture->target != GL_TEXTURE_2D_ARRAY ||
      !write->width || !write->height || write->x > gpu_texture->width ||
      write->y > gpu_texture->height ||
      write->width > gpu_texture->width - write->x ||
      write->height > gpu_texture->height - write->y ||
      write->layer >= gpu_texture->layers)
    return RN_ERROR_INVALID_ARGUMENT;

  uint64_t required_bytes = (uint64_t)write->width * write->height;

  if (required_bytes > SIZE_MAX / 4u)
    return RN_ERROR_OVERFLOW;

  required_bytes *= 4u;
  if (size < (size_t)required_bytes)
    return RN_ERROR_INVALID_ARGUMENT;

  glBindTexture(GL_TEXTURE_2D_ARRAY, gpu_texture->gl_id);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, (GLint)write->x, (GLint)write->y,
                  (GLint)write->layer, (GLsizei)write->width,
                  (GLsizei)write->height, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                  rgba_pixels);
  return _gl_check(ctx->log, __func__);
}

static void _destroy_texture(void *backend_ctx, rn_texture_handle_t handle) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx)
    return;

  struct rn_gl_texture *gpu_texture = _texture(ctx, handle);

  if (!gpu_texture)
    return;

  _RN_GL_LOG_TRACE(ctx->log, "destroying texture %u (GL=%u)",
                   (unsigned int)handle, (unsigned int)gpu_texture->gl_id);

  glDeleteTextures(1, &gpu_texture->gl_id);
  gpu_texture->alive = false;
}

static rn_result_t _set_texture(void *backend_ctx, uint32_t slot,
                                rn_texture_handle_t handle) {
  struct rn_gl_context *ctx = backend_ctx;

  if (!ctx)
    return RN_ERROR_INVALID_ARGUMENT;

  struct rn_gl_texture *gpu_texture = _texture(ctx, handle);
  GLint max_texture_units = 0;

  glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_texture_units);
  if (!gpu_texture || slot >= (uint32_t)max_texture_units)
    return RN_ERROR_INVALID_ARGUMENT;

  glActiveTexture(GL_TEXTURE0 + slot);
  glBindTexture(gpu_texture->target, gpu_texture->gl_id);
  return _gl_check(ctx->log, __func__);
}

bool rn_gl_backend_implement(struct rn_backend_impl_t *impl,
                             const struct rn_allocator_t *alloc,
                             const struct rn_log_t *log, const void *backend_info) {
  if (!impl || !alloc || !alloc->alloc || !alloc->realloc || !alloc->free)
    return false;

  _RN_GL_LOG_TRACE(log, "initializing OpenGL backend");

  if (!glad_loaded) {
    _RN_GL_LOG_TRACE(log, "loading OpenGL function pointers through GLAD");

    if (!backend_info) {
      _RN_GL_LOG_ERROR(log, "OpenGL loader was not provided");
      return false;
    }

    rn_gl_load_proc load_proc = *(const rn_gl_load_proc *)backend_info;

    if (!load_proc || !gladLoadGLLoader((GLADloadproc)load_proc)) {
      _RN_GL_LOG_ERROR(log, "failed to initialize GLAD, did you provide "
                            "rn_init_options_t.gl_load_proc?");
      return false;
    }

    glad_loaded = true;
    _RN_GL_LOG_TRACE(log, "GLAD function loading succeeded");
  } else {
    _RN_GL_LOG_TRACE(log, "reusing previously loaded GLAD functions");
  }

  if (!GLAD_GL_VERSION_3_3) {
    _RN_GL_LOG_ERROR(log,
                     "OpenGL 3.3 is unavailable or GLAD is not initialized");
    return false;
  }

  _RN_GL_LOG_TRACE(log, "OpenGL 3.3 support confirmed");
  _RN_GL_LOG_TRACE(log, "allocating OpenGL backend state");

  struct rn_gl_context *ctx = alloc->alloc(sizeof(*ctx), alloc->ctx);

  if (!ctx) {
    _RN_GL_LOG_ERROR(log, "failed to allocate OpenGL backend context");
    return false;
  }

  memset(ctx, 0, sizeof(*ctx));

  _RN_GL_LOG_TRACE(log, "creating backend vertex array");

  glGenVertexArrays(1, &ctx->vertex_array);
  if (!ctx->vertex_array) {
    _RN_GL_LOG_ERROR(log, "glGenVertexArrays failed");
    _gl_check(log, __func__);
    alloc->free(ctx, alloc->ctx);
    return false;
  }

  ctx->pipeline = RN_INVALID_HANDLE;
  ctx->index_buffer = RN_INVALID_HANDLE;
  for (size_t binding_index = 0; binding_index < RN_GL_MAX_BINDINGS;
       ++binding_index)
    ctx->bindings[binding_index].handle = RN_INVALID_HANDLE;

  ctx->alloc = alloc;
  ctx->log = log;

  *impl = _rn_gl_dispatch;
  impl->ctx = ctx;

  _RN_GL_LOG_TRACE(log, "OpenGL backend initialized (VAO=%u)",
                   (unsigned int)ctx->vertex_array);

  return true;
}

void rn_gl_backend_fini(struct rn_backend_impl_t *backend,
                        const struct rn_allocator_t *alloc) {
  if (!backend || !alloc || backend->create_buffer != _create_buffer)
    return;

  struct rn_gl_context *ctx = backend->ctx;

  if (!ctx)
    return;

  _RN_GL_LOG_TRACE(ctx->log,
                   "tearing down OpenGL backend (%zu buffer slots, %zu texture "
                   "slots, %zu pipeline slots)",
                   ctx->buffer_count, ctx->texture_count, ctx->pipeline_count);

  for (size_t resource_index = 0; resource_index < ctx->buffer_count;
       ++resource_index)
    if (ctx->buffers[resource_index].alive)
      glDeleteBuffers(1, &ctx->buffers[resource_index].gl_id);

  for (size_t resource_index = 0; resource_index < ctx->texture_count;
       ++resource_index)
    if (ctx->textures[resource_index].alive)
      glDeleteTextures(1, &ctx->textures[resource_index].gl_id);

  for (size_t resource_index = 0; resource_index < ctx->pipeline_count;
       ++resource_index)
    if (ctx->pipelines[resource_index].alive) {
      glDeleteProgram(ctx->pipelines[resource_index].gl_id);
      for (size_t binding_index = 0;
           binding_index < ctx->pipelines[resource_index].layout_count;
           ++binding_index)
        ctx->alloc->free(
            ctx->pipelines[resource_index].attributes[binding_index],
            ctx->alloc->ctx);
    }

  _RN_GL_LOG_TRACE(ctx->log, "destroying backend vertex array (VAO=%u)",
                   (unsigned int)ctx->vertex_array);

  glDeleteVertexArrays(1, &ctx->vertex_array);

  _RN_GL_LOG_TRACE(ctx->log, "releasing OpenGL backend CPU resources");

  ctx->alloc->free(ctx->buffers, ctx->alloc->ctx);
  ctx->alloc->free(ctx->textures, ctx->alloc->ctx);
  ctx->alloc->free(ctx->pipelines, ctx->alloc->ctx);

  _RN_GL_LOG_TRACE(ctx->log, "OpenGL backend teardown complete");
  ctx->alloc->free(ctx, ctx->alloc->ctx);

  memset(backend, 0, sizeof(*backend));
}
