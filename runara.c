#include "include/runara/runara.h"
#include <cglm/mat4.h>
#include <cglm/types-struct.h>

#include <glad/glad.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define HOMEDIR "USERPROFILE"
#else
#define HOMEDIR (char*)"HOME"
#endif

#define MAX(a, b) a > b ? a : b
#define MIN(a, b) a < b ? a : b

#define RN_TRACE(...) { printf("runara: [TRACE]: "); printf(__VA_ARGS__); printf("\n"); } 
#ifdef RN_DEBUG
#define RN_DEBUG(...) { printf("runara: [DEBUG]: "); printf(__VA_ARGS__); printf("\n"); } 
#else
#define RN_DEBUG(...) 
#endif // RN_DEBUG 
#define RN_INFO(...) { printf("runara: [INFO]: "); printf(__VA_ARGS__); printf("\n"); } 
#define RN_WARN(...) { printf("runara: [WARN]: "); printf(__VA_ARGS__); printf("\n"); } 
#define RN_ERROR(...) { printf("[runara ERROR]: "); printf(__VA_ARGS__); printf("\n"); } 

#ifdef _MSC_VER 
#define D_BREAK __debugbreak
#elif defined(__clang__) || defined(__GNUC__)
#define D_BREAK __builtin_trap
#else 
#define D_BREAK
#endif 

#ifdef _DEBUG 
#define RN_ASSERT(cond, ...) { if(cond) {} else { printf("[runara]: Assertion failed: '"); printf(__VA_ARGS__); printf("' in file '%s' on line %i.\n", __FILE__, __LINE__); D_BREAK(); }}
#else 
#define RN_ASSERT(cond, ...)
#endif // _DEBUG
//
// --- Renderer ---
static uint32_t   shader_create(GLenum type, const char* src);
static LfShader   shader_prg_create(const char* vert_src, const char* frag_src);
static void       shader_set_mat(LfShader prg, const char* name, mat4 mat); 
static void       set_projection_matrix(LfState* state);
static void       renderer_init(LfState* state);
static void       renderer_flush(LfState* state);
static void       renderer_begin(LfState* state);


static void       init_glyph_cache(LfGlyphCache* cache, size_t init_cap);
static void       resize_glyph_cache(LfGlyphCache* cache, size_t new_cap);
static void       add_glyph_to_cache(LfGlyphCache* cache, LfGlyph glyph);
static void       free_glyph_cache(LfGlyphCache* cache);
static LfGlyph*   get_glyph_from_codepoint(LfGlyphCache cache, uint64_t codepoint);
static LfGlyph    load_glyph_from_codepoint(LfFont font, uint64_t codepoint);
static LfGlyph    get_glyph_from_cache(LfGlyphCache* cache, LfFont font, uint64_t codepoint);

static void               init_hb_cache(LfHarfbuzzCache* cache, size_t init_cap);
static void               resize_hb_cache(LfHarfbuzzCache* cache, size_t new_cap);
static void               add_text_to_hb_cache(LfHarfbuzzCache* cache, LfHarfbuzzText text); 
static void               free_hb_cache(LfHarfbuzzCache* cache);
static LfHarfbuzzText*    get_hb_text_from_str(LfHarfbuzzCache cache, const char* str);
static LfHarfbuzzText     load_hb_text_from_str(LfFont font, const char* str);
static LfHarfbuzzText     get_hb_text_from_cache(LfHarfbuzzCache* cache, LfFont font, const char* str);

static uint64_t           djb2_hash(const unsigned char *str);

// --- Static Functions --- 
uint32_t shader_create(GLenum type, const char* src) {
  // Create && compile the shader with opengl 
  uint32_t shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, NULL);
  glCompileShader(shader);

  // Check for compilation errors
  int32_t compiled; 
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

  if(!compiled) {
    RN_ERROR("Failed to compile %s shader.", type == GL_VERTEX_SHADER ? "vertex" : "fragment");
    char info[512];
    glGetShaderInfoLog(shader, 512, NULL, info);
    RN_INFO("%s", info);
    glDeleteShader(shader);
  }
  return shader;
}

LfShader shader_prg_create(const char* vert_src, const char* frag_src) {
  // Creating vertex & fragment shader with the shader API
  uint32_t vertex_shader = shader_create(GL_VERTEX_SHADER, vert_src);
  uint32_t fragment_shader = shader_create(GL_FRAGMENT_SHADER, frag_src);

  // Creating & linking the shader program with OpenGL
  LfShader prg;
  prg.id = glCreateProgram();
  glAttachShader(prg.id, vertex_shader);
  glAttachShader(prg.id, fragment_shader);
  glLinkProgram(prg.id);

  // Check for linking errors
  int32_t linked;
  glGetProgramiv(prg.id, GL_LINK_STATUS, &linked);

  if(!linked) {
    RN_ERROR("Failed to link shader program.");
    char info[512];
    glGetProgramInfoLog(prg.id, 512, NULL, info);
    RN_INFO("%s", info);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    glDeleteProgram(prg.id);
    return prg;
  }

  // Delete the shaders after
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
  return prg;
}

void shader_set_mat(LfShader prg, const char* name, mat4 mat) {
  glUniformMatrix4fv(glGetUniformLocation(prg.id, name), 1, GL_FALSE, mat[0]);
}

void set_projection_matrix(LfState* state) { 
  float left = 0.0f;
  float right = state->render_w;
  float bottom = state->render_h;
  float top = 0.0f;
  float near = 0.1f;
  float far = 100.0f;

  // Create the orthographic projection matrix
  mat4 orthoMatrix = GLM_MAT4_IDENTITY_INIT;
  orthoMatrix[0][0] = 2.0f / (right - left);
  orthoMatrix[1][1] = 2.0f / (top - bottom);
  orthoMatrix[2][2] = -1;
  orthoMatrix[3][0] = -(right + left) / (right - left);
  orthoMatrix[3][1] = -(top + bottom) / (top - bottom);

  shader_set_mat(state->render.shader, "u_proj", orthoMatrix);
}

void renderer_init(LfState* state) {
  // OpenGL Setup 
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  state->render.vert_count = 0;
  state->render.verts = (LfVertex*)malloc(sizeof(LfVertex) * RN_MAX_RENDER_BATCH * 4);

  /* Creating vertex array & vertex buffer for the batch renderer */
  glCreateVertexArrays(1, &state->render.vao);
  glBindVertexArray(state->render.vao);

  glCreateBuffers(1, &state->render.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, state->render.vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(LfVertex) * RN_MAX_RENDER_BATCH * 4, NULL, 
               GL_DYNAMIC_DRAW);

  uint32_t* indices = (uint32_t*)malloc(sizeof(uint32_t) * RN_MAX_RENDER_BATCH * 6);

  uint32_t offset = 0;
  for (uint32_t i = 0; i < RN_MAX_RENDER_BATCH * 6; i += 6) {
    indices[i + 0] = offset + 0;
    indices[i + 1] = offset + 1;
    indices[i + 2] = offset + 2;

    indices[i + 3] = offset + 2;
    indices[i + 4] = offset + 3;
    indices[i + 5] = offset + 0;
    offset += 4;
  }
  glCreateBuffers(1, &state->render.ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state->render.ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, RN_MAX_RENDER_BATCH * 6 * sizeof(uint32_t), indices, GL_STATIC_DRAW);

  free(indices); 
  // Setting the vertex layout
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(LfVertex), NULL);
  glEnableVertexAttribArray(0);

  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t)offsetof(LfVertex, border_color));
  glEnableVertexAttribArray(1);

  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t)offsetof(LfVertex, border_width));
  glEnableVertexAttribArray(2);

  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t)offsetof(LfVertex, color));
  glEnableVertexAttribArray(3);

  glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t*)offsetof(LfVertex, texcoord));
  glEnableVertexAttribArray(4);

  glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t*)offsetof(LfVertex, tex_index));
  glEnableVertexAttribArray(5);

  glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t*)offsetof(LfVertex, scale));
  glEnableVertexAttribArray(6);

  glVertexAttribPointer(7, 2, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t*)offsetof(LfVertex, pos_px));
  glEnableVertexAttribArray(7);

  glVertexAttribPointer(8, 1, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t*)offsetof(LfVertex, corner_radius));
  glEnableVertexAttribArray(8);

  glVertexAttribPointer(10, 2, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t*)offsetof(LfVertex, min_coord));
  glEnableVertexAttribArray(10);

  glVertexAttribPointer(11, 2, GL_FLOAT, GL_FALSE, sizeof(LfVertex), (void*)(intptr_t*)offsetof(LfVertex, max_coord));
  glEnableVertexAttribArray(11);

  // Creating the shader for the batch renderer
  const char* vert_src =
    "#version 450 core\n"
    "layout (location = 0) in vec2 a_pos;\n"
    "layout (location = 1) in vec4 a_border_color;\n"
    "layout (location = 2) in float a_border_width;\n"
    "layout (location = 3) in vec4 a_color;\n"
    "layout (location = 4) in vec2 a_texcoord;\n"
    "layout (location = 5) in float a_tex_index;\n"
    "layout (location = 6) in vec2 a_scale;\n"
    "layout (location = 7) in vec2 a_pos_px;\n"
    "layout (location = 8) in float a_corner_radius;\n"
    "layout (location = 10) in vec2 a_min_coord;\n"
    "layout (location = 11) in vec2 a_max_coord;\n"

    "uniform mat4 u_proj;\n"
    "out vec4 v_border_color;\n"
    "out float v_border_width;\n"
    "out vec4 v_color;\n"
    "out vec2 v_texcoord;\n"
    "out float v_tex_index;\n"
    "flat out vec2 v_scale;\n"
    "flat out vec2 v_pos_px;\n"
    "flat out float v_is_gradient;\n"
    "out float v_corner_radius;\n"
    "out vec2 v_min_coord;\n"
    "out vec2 v_max_coord;\n"

    "void main() {\n"
    "v_color = a_color;\n"
    "v_texcoord = a_texcoord;\n"
    "v_tex_index = a_tex_index;\n"
    "v_border_color = a_border_color;\n"
    "v_border_width = a_border_width;\n"
    "v_scale = a_scale;\n"
    "v_pos_px = a_pos_px;\n"
    "v_corner_radius = a_corner_radius;\n"
    "v_min_coord = a_min_coord;\n"
    "v_max_coord = a_max_coord;\n"
    "gl_Position = u_proj * vec4(a_pos.x, a_pos.y, 0.0f, 1.0);\n"
    "}\n";


  const char* frag_src = "#version 450 core\n"
    "out vec4 o_color;\n"
    "in vec4 v_color;\n"
    "in float v_tex_index;\n"
    "in vec4 v_border_color;\n"
    "in float v_border_width;\n"
    "in vec2 v_texcoord;\n"
    "flat in vec2 v_scale;\n"
    "flat in vec2 v_pos_px;\n"
    "in float v_corner_radius;\n"
    "uniform sampler2D u_textures[32];\n"
    "uniform vec2 u_screen_size;\n"
    "in vec2 v_min_coord;\n"
    "in vec2 v_max_coord;\n"

    "float rounded_box_sdf(vec2 center_pos, vec2 size, float radius) {\n"
    "    return length(max(abs(center_pos)-size+radius,0.0))-radius;\n"
    "}\n"

    "void main() {\n"
    "     if(u_screen_size.y - gl_FragCoord.y < v_min_coord.y && v_min_coord.y != -1) {\n"
    "         discard;\n"
    "     }\n"
    "     if(u_screen_size.y - gl_FragCoord.y > v_max_coord.y && v_max_coord.y != -1) {\n"
    "         discard;\n"
    "     }\n"
    "     if ((gl_FragCoord.x < v_min_coord.x && v_min_coord.x != -1) || (gl_FragCoord.x > v_max_coord.x && v_max_coord.x != -1)) {\n"
    "         discard;\n" 
    "     }\n"
    "     vec2 size = v_scale;\n"
    "     vec4 opaque_color, display_color;\n"
    "     if(v_tex_index == -1) {\n"
    "       opaque_color = v_color;\n"
    "     } else {\n"
    "       opaque_color = texture(u_textures[int(v_tex_index)], v_texcoord) * v_color;\n"
    "     }\n"
    "     if(v_corner_radius != 0.0f) {"
    "       display_color = opaque_color;\n"
    "       vec2 location = vec2(v_pos_px.x, -v_pos_px.y);\n"
    "       location.y += u_screen_size.y - size.y;\n"
    "       float edge_softness = 1.0f;\n"
    "       float radius = v_corner_radius * 2.0f;\n"
    "       float distance = rounded_box_sdf(gl_FragCoord.xy - location - (size/2.0f), size / 2.0f, radius);\n"
    "       float smoothed_alpha = 1.0f-smoothstep(0.0f, edge_softness * 2.0f,distance);\n"
    "       vec3 fill_color;\n"
    "       if(v_border_width != 0.0f) {\n"
    "           vec2 location_border = vec2(location.x + v_border_width, location.y + v_border_width);\n"
    "           vec2 size_border = vec2(size.x - v_border_width * 2, size.y - v_border_width * 2);\n"
    "           float distance_border = rounded_box_sdf(gl_FragCoord.xy - location_border - (size_border / 2.0f), size_border / 2.0f, radius);\n"
    "           if(distance_border <= 0.0f) {\n"
    "               fill_color = display_color.xyz;\n"
    "           } else {\n"
    "               fill_color = v_border_color.xyz;\n"
    "           }\n"
    "       } else {\n"
    "           fill_color = display_color.xyz;\n"
    "       }\n"
    "       if(v_border_width != 0.0f)\n" 
    "         o_color =  mix(vec4(0.0f, 0.0f, 0.0f, 0.0f), vec4(fill_color, smoothed_alpha), smoothed_alpha);\n"
    "       else\n" 
    "         o_color = mix(vec4(0.0f, 0.0f, 0.0f, 0.0f), vec4(fill_color, display_color.a), smoothed_alpha);\n"
    "     } else {\n"
    "       vec4 fill_color = opaque_color;\n"
    "       if(v_border_width != 0.0f) {\n"
    "           vec2 location = vec2(v_pos_px.x, -v_pos_px.y);\n"
    "           location.y += u_screen_size.y - size.y;\n"
    "           vec2 location_border = vec2(location.x + v_border_width, location.y + v_border_width);\n"
    "           vec2 size_border = vec2(v_scale.x - v_border_width * 2, v_scale.y - v_border_width * 2);\n"
    "           float distance_border = rounded_box_sdf(gl_FragCoord.xy - location_border - (size_border / 2.0f), size_border / 2.0f, v_corner_radius);\n"
    "           if(distance_border > 0.0f) {\n"
    "               fill_color = v_border_color;\n"
    "}\n"
    "       }\n"
    "       o_color = fill_color;\n"
    " }\n"
    "}\n";
  state->render.shader = shader_prg_create(vert_src, frag_src);

  // initializing vertex position data
  state->render.vert_pos[0] = (vec4s){-0.5f, -0.5f, 0.0f, 1.0f};
  state->render.vert_pos[1] = (vec4s){0.5f, -0.5f, 0.0f, 1.0f};
  state->render.vert_pos[2] = (vec4s){0.5f, 0.5f, 0.0f, 1.0f};
  state->render.vert_pos[3] = (vec4s){-0.5f, 0.5f, 0.0f, 1.0f};


  // Populating the textures array in the shader with texture ids
  int32_t tex_slots[RN_MAX_TEX_COUNT_BATCH];
  for(uint32_t i = 0; i < RN_MAX_TEX_COUNT_BATCH; i++) 
    tex_slots[i] = i;

  glUseProgram(state->render.shader.id);
  set_projection_matrix(state);
  glUniform1iv(glGetUniformLocation(state->render.shader.id, "u_textures"), RN_MAX_TEX_COUNT_BATCH, tex_slots);
}

void renderer_flush(LfState* state) {
  if(state->render.vert_count <= 0) return;

  // Bind the vertex buffer & shader set the vertex data, bind the textures & draw
  glUseProgram(state->render.shader.id);
  glBindBuffer(GL_ARRAY_BUFFER, state->render.vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(LfVertex) * state->render.vert_count, 
                  state->render.verts);

  for(uint32_t i = 0; i < state->render.tex_count; i++) {
    glBindTextureUnit(i, state->render.textures[i].id);
    state->drawcalls++;
  }

  vec2s renderSize = (vec2s){(float)state->render_w, (float)state->render_h};
  glUniform2fv(glGetUniformLocation(state->render.shader.id, "u_screen_size"), 1, (float*)renderSize.raw);
  glBindVertexArray(state->render.vao);
  glDrawElements(GL_TRIANGLES, state->render.index_count, GL_UNSIGNED_INT, NULL);
}

void renderer_begin(LfState* state) {
  state->render.vert_count = 0;
  state->render.index_count = 0;
  state->render.tex_index = 0;
  state->render.tex_count = 0;
  state->drawcalls = 0;
}

void init_glyph_cache(LfGlyphCache* cache, size_t init_cap) {
  cache->glyphs = malloc(init_cap * sizeof(*cache->glyphs));
  cache->size   = 0;
  cache->cap    = init_cap;
}

void resize_glyph_cache(LfGlyphCache* cache, size_t new_cap) {
  LfGlyph* tmp = (LfGlyph*)realloc(cache->glyphs, new_cap * sizeof(*cache->glyphs));
  if(tmp) {
    cache->glyphs = tmp;
    cache->cap = new_cap;
  } else {
    RN_ERROR("Failed to allocate memory for glyph in cache.");
  }
}

void add_glyph_to_cache(LfGlyphCache* cache, LfGlyph glyph) {
  if(cache->size == cache->cap) {
    resize_glyph_cache(cache, cache->cap * 2);
  }
  cache->glyphs[cache->size++] = glyph;
}

void free_glyph_cache(LfGlyphCache* cache) {
  free(cache->glyphs);
  cache->glyphs = NULL;
  cache->size   = 0;
  cache->cap    = 0;
}

LfGlyph* get_glyph_from_codepoint(LfGlyphCache cache, uint64_t codepoint) {
  for(uint32_t i = 0; i < cache.size; i++) {
    if(cache.glyphs[i].codepoint == codepoint) {
      return &cache.glyphs[i];
    }
  }
  return NULL;
}

LfGlyph load_glyph_from_codepoint(LfFont font, uint64_t codepoint) {
  LfGlyph glyph;
  if (FT_Load_Glyph(font.face, codepoint, FT_LOAD_RENDER)) {
    RN_ERROR("Failed to load glyph of character with codepoint '%i'.", codepoint);
    return glyph;
  }
  FT_GlyphSlot slot = font.face->glyph;
  int glyph_height = slot->metrics.height >> 6; // Convert from 26.6 fixed-point to pixels

  // Get the bitmap from the glyph
  FT_Bitmap* bitmap = &font.face->glyph->bitmap;

  // Allocate memory for RGBA buffer
  int width = bitmap->width;
  int height = bitmap->rows;
  unsigned char* rgba_buf = (unsigned char*)malloc(width * height * 4); // 4 bytes per pixel (RGBA)

  if (!rgba_buf) {
    RN_ERROR("Failed to allocate memory for RGBA buffer.");
    return glyph;
  }

  // Fill the RGBA buffer with the glyph bitmap data
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      unsigned char grayscale = bitmap->buffer[y * bitmap->pitch + x];
      int index = (y * width + x) * 4;
      rgba_buf[index] = grayscale;     // Red
      rgba_buf[index + 1] = grayscale; // Green
      rgba_buf[index + 2] = grayscale; // Blue
      rgba_buf[index + 3] = grayscale; // Alpha
    }
  }

  glGenTextures(1, &glyph.id);
  glBindTexture(GL_TEXTURE_2D, glyph.id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTextureParameteri(glyph.id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTextureParameteri(glyph.id, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);

  glTexImage2D(
    GL_TEXTURE_2D,
    0,
    GL_RGBA,
    width,
    height,
    0,
    GL_RGBA,
    GL_UNSIGNED_BYTE,
    rgba_buf
  );

  glGenerateMipmap(GL_TEXTURE_2D);

  glyph.width = width;
  glyph.height = glyph_height;
  glyph.bearing_x = font.face->glyph->bitmap_left;
  glyph.bearing_y = font.face->glyph->bitmap_top;
  glyph.advance = font.face->glyph->advance.x;
  glyph.codepoint = codepoint;

  // Clean up the RGBA buffer
  free(rgba_buf);

  printf("Added glyph %i to glyph cache.\n", codepoint);
  return glyph;
}

LfGlyph get_glyph_from_cache(LfGlyphCache* cache, LfFont font, uint64_t codepoint) {
  LfGlyph* glyph = get_glyph_from_codepoint(*cache, codepoint);

  if(glyph) {
    return *glyph;
  }

  LfGlyph new_glyph = load_glyph_from_codepoint(font, codepoint);
  add_glyph_to_cache(cache, new_glyph);
  return new_glyph; 
}

void init_hb_cache(LfHarfbuzzCache* cache, size_t init_cap) {
  cache->texts  = malloc(init_cap * sizeof(*cache->texts));
  cache->size   = 0;
  cache->cap    = init_cap;
}

void resize_hb_cache(LfHarfbuzzCache* cache, size_t new_cap) {
  LfHarfbuzzText* tmp = (LfHarfbuzzText*)realloc(cache->texts, new_cap * sizeof(*cache->texts));
  if(tmp) {
    cache->texts = tmp;
    cache->cap = new_cap;
  } else {
    RN_ERROR("Failed to allocate memory for harfbuzz text in cache.");
  }

}
void add_text_to_hb_cache(LfHarfbuzzCache* cache, LfHarfbuzzText text) {
  if(cache->size == cache->cap) {
    resize_hb_cache(cache, cache->cap * 2);
  }
  cache->texts[cache->size++] = text;
}

void free_hb_cache(LfHarfbuzzCache* cache) {
  for(uint32_t i = 0; i < cache->size; i++) {
    hb_buffer_destroy(cache->texts[i].buf);
  }
  free(cache->texts);
  cache->texts  = NULL;
  cache->size   = 0;
  cache->cap    = 0;
}

LfHarfbuzzText* get_hb_text_from_str(LfHarfbuzzCache cache, const char* str) {
  uint64_t hash = djb2_hash(str);
  for(uint32_t i = 0; i < cache.size; i++) {
    if(cache.texts[i].hash == hash) {
      return &cache.texts[i];
    }
  }
  return NULL;
}

LfHarfbuzzText load_hb_text_from_str(LfFont font, const char* str) {
  LfHarfbuzzText text;

  // Create a HarfBuzz buffer and add text
  text.buf = hb_buffer_create();
  hb_buffer_add_utf8(text.buf, str, -1, 0, -1);

  // Shape the text
  hb_buffer_guess_segment_properties(text.buf);
  hb_shape(font.hb_font, text.buf, NULL, 0);

  int32_t len;
  // Retrieve glyph information and positions
  text.glyph_info = hb_buffer_get_glyph_infos(text.buf, &text.glyph_count);
  text.glyph_pos = hb_buffer_get_glyph_positions(text.buf, &text.glyph_count);

  text.hash = djb2_hash(str);

  printf("Added text '%s' to harfbuzz cache.\n", str);
  return text;

}

LfHarfbuzzText get_hb_text_from_cache(LfHarfbuzzCache* cache, LfFont font, const char* str) {
  LfHarfbuzzText* glyph = get_hb_text_from_str(*cache, str);

  if(glyph) {
    return *glyph;
  }

  LfHarfbuzzText new_text = load_hb_text_from_str(font, str);
  add_text_to_hb_cache(cache, new_text);
  return new_text; 
}

uint64_t djb2_hash(const unsigned char *str) {
  unsigned long hash = 5381;
  int c;

  while ((c = *str++)) {
    hash = ((hash << 5) + hash) + c;  // hash * 33 + c
  }

  return hash;
}
// ===========================================================
// ----------------Public API Functions ---------------------- 
// ===========================================================
LfState rn_init(uint32_t render_w, uint32_t render_h, LfGLLoader loader) {
  LfState state;
  setlocale(LC_ALL, "");

  if(!gladLoadGLLoader((GLADloadproc)loader)) {
    RN_ERROR("Failed to initialize Glad.");
    return state;
  }

  memset(&state, 0, sizeof(state));
  // Default state
  state.init = true;
  state.render_w = render_w;
  state.render_h = render_h;
  state.render.tex_count = 0;

  state.drawcalls = 0;

  state.cull_start = (vec2s){-1, -1};
  state.cull_end = (vec2s){-1, -1};


  if(FT_Init_FreeType(&state.ft) != 0) {
    RN_ERROR("Failed to initialize FreeType.");
  }

  init_glyph_cache(&state.glyph_cache, 32);
  init_hb_cache(&state.hb_cache, 32);

  renderer_init(&state);

  return state;
}

void rn_terminate(LfState* state) {
  free_glyph_cache(&state->glyph_cache);
  free_hb_cache(&state->hb_cache);
  FT_Done_FreeType(state->ft);
}

void rn_resize_display(LfState* state, uint32_t render_w, uint32_t render_h) {
  state->render_w = render_w;
  state->render_h = render_h;

  glViewport(0, 0, render_w, render_h);
  set_projection_matrix(state);
}

LfTexture rn_load_texture(const char* filepath, bool flip, LfTextureFiltering filter) {
  LfTexture tex;
  int width, height, channels;
  unsigned char* image = stbi_load(filepath, &width, &height, &channels, STBI_rgb_alpha);
  if (!image) {
    RN_ERROR("Failed to load texture at '%s'.", filepath);
    return tex;
  }

  glGenTextures(1, &tex.id);
  glBindTexture(GL_TEXTURE_2D, tex.id); 

  // Set texture parameters
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  switch(filter) {
    case RN_TEX_FILTER_LINEAR:
      glTextureParameteri(tex.id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTextureParameteri(tex.id, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      break;
    case RN_TEX_FILTER_NEAREST:
      glTextureParameteri(tex.id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTextureParameteri(tex.id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      break;
  }

  // Load texture data
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image);
  glGenerateMipmap(GL_TEXTURE_2D);
  stbi_image_free(image); // Free image data 
  //
  tex.width = width;
  tex.height = height;
  return tex;
}

LfFont rn_load_font(LfState* state, const char* filepath, uint32_t size, uint32_t line_height) {
  LfFont font;
  FT_Face face;
  if(FT_New_Face(state->ft, filepath, 0, &face)) {
    RN_ERROR("Failed to load font file '%s'.", filepath);
    return font;
  }

  FT_Set_Pixel_Sizes(face, 0, size);

  font.face = face;
  font.size = size;
  font.line_height = line_height;

  font.hb_font = hb_ft_font_create(font.face, NULL);

  return font;
}

void rn_free_texture(LfTexture* tex) {
  glDeleteTextures(1, &tex->id);
  memset(tex, 0, sizeof(*tex));
}

void rn_free_font(LfFont* font) {
  FT_Done_Face(font->face);
  hb_font_destroy(font->hb_font);
  memset(font, 0, sizeof(*font));
}

void rn_begin(LfState* state) {
  renderer_begin(state);
}
void rn_end(LfState* state) {
  renderer_flush(state);
  state->drawcalls = 0;
}


void rn_rect_render(LfState* state, vec2s pos, vec2s size, LfColor color, LfColor border_color, float border_width, float corner_radius) {
  // Offsetting the postion, so that pos is the top left of the rendered object
  vec2s pos_initial = pos;
  pos = (vec2s){pos.x + size.x / 2.0f, pos.y + size.y / 2.0f};

  // Initializing texture coords data
  vec2s texcoords[4] = {
    (vec2s){1.0f, 1.0f},
    (vec2s){1.0f, 0.0f},
    (vec2s){0.0f, 0.0f},
    (vec2s){0.0f, 1.0f},
  };
  // Calculating the transform matrix
  mat4 translate; 
  mat4 scale;
  mat4 transform;
  vec3 pos_xyz = {(corner_radius != 0.0f ? 
    (float)state->render_w / 2.0f : pos.x), 
    (corner_radius != 0.0f ? (float)state->render_h / 2.0f : pos.y),
    0.0f};

  vec3 size_xyz = {corner_radius != 0.0f ? state->render_w : size.x, 
    corner_radius != 0.0f ? state->render_h : size.y, 
    0.0f};
  glm_translate_make(translate, pos_xyz);
  glm_scale_make(scale, size_xyz);
  glm_mat4_mul(translate,scale,transform);

  // Adding the vertices to the batch renderer
  for(uint32_t i = 0; i < 4; i++) {
    if(state->render.vert_count >= RN_MAX_RENDER_BATCH) {
      renderer_flush(state);
      renderer_begin(state);
    }
    vec4 result;
    glm_mat4_mulv(transform, state->render.vert_pos[i].raw, result);
    state->render.verts[state->render.vert_count].pos[0] = result[0];
    state->render.verts[state->render.vert_count].pos[1] = result[1];

    vec4s border_color_zto = rn_color_to_zto(border_color);
    const vec4 border_color_arr = {border_color_zto.r, border_color_zto.g, border_color_zto.b, border_color_zto.a};
    memcpy(state->render.verts[state->render.vert_count].border_color, border_color_arr, sizeof(vec4));

    state->render.verts[state->render.vert_count].border_width = border_width; 

    vec4s color_zto = rn_color_to_zto(color);
    const vec4 color_arr = {color_zto.r, color_zto.g, color_zto.b, color_zto.a};
    memcpy(state->render.verts[state->render.vert_count].color, color_arr, sizeof(vec4));

    const vec2 texcoord_arr = {texcoords[i].x, texcoords[i].y};
    memcpy(state->render.verts[state->render.vert_count].texcoord, texcoord_arr, sizeof(vec2));

    state->render.verts[state->render.vert_count].tex_index = -1;

    const vec2 scale_arr = {size.x, size.y};
    memcpy(state->render.verts[state->render.vert_count].scale, scale_arr, sizeof(vec2));

    const vec2 pos_px_arr = {(float)pos_initial.x, (float)pos_initial.y};
    memcpy(state->render.verts[state->render.vert_count].pos_px, pos_px_arr, sizeof(vec2));

    state->render.verts[state->render.vert_count].corner_radius = corner_radius;

    const vec2 cull_start_arr = {state->cull_start.x, state->cull_start.y};
    memcpy(state->render.verts[state->render.vert_count].min_coord, cull_start_arr, sizeof(vec2));

    const vec2 cull_end_arr = {state->cull_end.x, state->cull_end.y};
    memcpy(state->render.verts[state->render.vert_count].max_coord, cull_end_arr, sizeof(vec2));

    state->render.vert_count++;
  }
  state->render.index_count += 6;
}

void rn_image_render(LfState* state, vec2s pos, LfColor color, LfTexture tex, LfColor border_color, float border_width, float corner_radius) {
  // Check if we need to flush and start a new batch
  if (state->render.tex_count >= RN_MAX_TEX_COUNT_BATCH) {
    renderer_flush(state);
    renderer_begin(state);
  }

  // Adjust position to be the top-left of the rendered object
  vec2s pos_initial = pos;
  pos.x += tex.width / 2.0f;
  pos.y += tex.height / 2.0f;

  // Find or add texture and get its index
  float tex_index = -1.0f;
  for (uint32_t i = 0; i < state->render.tex_count; ++i) {
    if (tex.id == state->render.textures[i].id) {
      tex_index = i;
      break;
    }
  }
  if (tex_index == -1.0f) {
    tex_index = (float)state->render.tex_index;
    state->render.textures[state->render.tex_count++] = tex;
    state->render.tex_index++;
  }

  mat4 translate, scale, transform;
  vec3s pos_xyz = {pos.x, pos.y, 0.0f};
  vec3 tex_size = {tex.width, tex.height, 0.0f};

  glm_translate_make(translate, pos_xyz.raw);
  glm_scale_make(scale, tex_size);
  glm_mat4_mul(translate, scale, transform);

  // Precompute common values
  vec4s border_color_zto = rn_color_to_zto(border_color);
  vec4 border_color_arr = {
    border_color_zto.r,
    border_color_zto.g,
    border_color_zto.b,
    border_color_zto.a
  };

  vec4s color_zto = rn_color_to_zto(color); 
  vec4 color_arr = {
    color_zto.r,
    color_zto.g,
    color_zto.b,
    color_zto.a
  };

  vec2 texcoords[4] = { {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f} };
  vec2 scale_arr = { (float)tex.width, (float)tex.height };
  vec2 pos_px_arr = { (float)pos_initial.x, (float)pos_initial.y };
  vec2 cull_start_arr = { state->cull_start.x, state->cull_start.y };
  vec2 cull_end_arr = { state->cull_end.x, state->cull_end.y };

  // Add vertices
  for (uint32_t i = 0; i < 4; ++i) {
    if (state->render.vert_count >= RN_MAX_RENDER_BATCH) {
      renderer_flush(state);
      renderer_begin(state);
    }

    vec4 result;
    glm_mat4_mulv(transform, state->render.vert_pos[i].raw, result);

    LfVertex* vertex = &state->render.verts[state->render.vert_count];
    vertex->pos[0] = result[0];
    vertex->pos[1] = result[1];
    vertex->border_color[0] = border_color_arr[0];
    vertex->border_color[1] = border_color_arr[1];
    vertex->border_color[2] = border_color_arr[2];
    vertex->border_color[3] = border_color_arr[3];
    vertex->border_width = border_width;
    vertex->color[0] = color_arr[0];
    vertex->color[1] = color_arr[1];
    vertex->color[2] = color_arr[2];
    vertex->color[3] = color_arr[3];
    vertex->texcoord[0] = texcoords[i][0];
    vertex->texcoord[1] = texcoords[i][1];
    vertex->tex_index = tex_index;
    vertex->scale[0] = scale_arr[0];
    vertex->scale[1] = scale_arr[1];
    vertex->pos_px[0] = pos_px_arr[0];
    vertex->pos_px[1] = pos_px_arr[1];
    vertex->corner_radius = corner_radius;
    vertex->min_coord[0] = cull_start_arr[0];
    vertex->min_coord[1] = cull_start_arr[1];
    vertex->max_coord[0] = cull_end_arr[0];
    vertex->max_coord[1] = cull_end_arr[1];

    state->render.vert_count++;
  }

  state->render.index_count += 6;
}

LfTextProps rn_text_render_ex(LfState* state, 
                           const char* text, 
                           LfFont font, 
                              vec2s pos, 
                              LfColor color, 
                              bool render) {

  LfHarfbuzzText hb_text = get_hb_text_from_cache(&state->hb_cache, font, text);

  float highest_bearing = 0.0f;

  LfGlyph glyphs[hb_text.glyph_count];

  for (unsigned int i = 0; i < hb_text.glyph_count; i++) {
    glyphs[i] = rn_glyph_from_codepoint(state, 
                                        font,
                                        hb_text.glyph_info[i].codepoint); 
  }

  for (unsigned int i = 0; i < hb_text.glyph_count; i++) {
    LfGlyph glyph = glyphs[i];
    if(glyph.bearing_y > highest_bearing) {
      highest_bearing = glyph.bearing_y;
    }
  }

  vec2s start_pos = pos;

  const char* ptr = text;
  for (unsigned int i = 0; i < hb_text.glyph_count; i++) {
    uint64_t codepoint;
    int32_t len = rn_utf8_to_codepoint(ptr, &codepoint);
    if(!len) {
      ptr++;
      continue;
    }
    if(codepoint == '\n') {
      pos.y += font.line_height;
      pos.x = start_pos.x;
      ptr += len;
      continue;
    }
    LfGlyph glyph = glyphs[i];
    // Calculate position
    float x_advance = hb_text.glyph_pos[i].x_advance / 64.0f; 
    float y_advance = hb_text.glyph_pos[i].y_advance / 64.0f;
    float x_offset = hb_text.glyph_pos[i].x_offset / 64.0f;
    float y_offset = hb_text.glyph_pos[i].y_offset / 64.0f;

    vec2s glyph_pos = {
      pos.x + x_offset,
      pos.y + highest_bearing - y_offset 
    };

    // Render the glyph
    rn_glyph_render(state, glyph, font, glyph_pos, color);

    // Advance to the next glyph
    pos.x += x_advance;
    pos.y += y_advance;

    ptr += len;
  }


  return (LfTextProps){
    .width = pos.x - start_pos.x, 
    .height = pos.y - start_pos.y
  };
}

void rn_glyph_render(
    LfState* state,
    LfGlyph glyph,
    LfFont font, 
    vec2s pos,
    LfColor color) {

  float xpos = pos.x + glyph.bearing_x;
  float ypos = pos.y - glyph.bearing_y;

  LfTexture tex = (LfTexture){
    .id = glyph.id,
    .width = glyph.width, 
    .height = glyph.height
  };

  rn_image_render(state, (vec2s){xpos, ypos}, 
                  color, tex, RN_NO_COLOR, 0.0f, 0.0f);
}

LfGlyph rn_glyph_from_codepoint(
    LfState* state, 
    LfFont font, 
    uint64_t codepoint
    ) {
  return get_glyph_from_cache(&state->glyph_cache, font, codepoint);
}

LfTextProps rn_text_render(
    LfState* state, 
    const char* text,
    LfFont font, 
    vec2s pos, 
    LfColor color) {
  return rn_text_render_ex(state, text, font, pos, color, true);
}

LfTextProps rn_text_props(
    LfState* state, 
    const char* text, 
    LfFont font
    ) {
  return rn_text_render_ex(state, text, font, (vec2s){0, 0},
                           RN_NO_COLOR, false);
}

int32_t rn_utf8_to_codepoint(const char *utf8, uint64_t *codepoint) {
  unsigned char c = (unsigned char)*utf8;

  if (c <= 0x7F) {
    // 1-byte sequence (ASCII)
    *codepoint = c;
    return 1;
  } else if ((c >> 5) == 0x6) {
    // 2-byte sequence
    if ((utf8[1] & 0xC0) != 0x80) return -1;
    *codepoint = ((c & 0x1F) << 6) | (utf8[1] & 0x3F);
    return 2;
  } else if ((c >> 4) == 0xE) {
    // 3-byte sequence
    if ((utf8[1] & 0xC0) != 0x80 || (utf8[2] & 0xC0) != 0x80) return -1;
    *codepoint = ((c & 0x0F) << 12) | ((utf8[1] & 0x3F) << 6) | (utf8[2] & 0x3F);
    return 3;
  } else if ((c >> 3) == 0x1E) {
    // 4-byte sequence
    if ((utf8[1] & 0xC0) != 0x80 || (utf8[2] & 0xC0) != 0x80 || (utf8[3] & 0xC0) != 0x80) return -1;
    *codepoint = ((c & 0x07) << 18) | ((utf8[1] & 0x3F) << 12) | ((utf8[2] & 0x3F) << 6) | (utf8[3] & 0x3F);
    return 4;
  } else {
    // Invalid UTF-8
    return -1;
  }
}

void rn_set_cull_end_x(LfState* state, float x) {
  state->cull_end.x = x; 
}

void rn_set_cull_end_y(LfState* state, float y) {
  state->cull_end.y = y; 
}
void rn_set_cull_start_x(LfState* state, float x) {
  state->cull_start.x = x;
}

void rn_set_cull_start_y(LfState* state, float y) {
  state->cull_start.y = y;
}

void rn_unset_cull_start_x(LfState* state) {
  state->cull_start.x = -1;
}

void rn_unset_cull_start_y(LfState* state) {
  state->cull_start.y = -1;
}

void rn_unset_cull_end_x(LfState* state) {
  state->cull_end.x = -1;
}

void rn_unset_cull_end_y(LfState* state) {
  state->cull_end.y = -1;
}

LfColor rn_color_brightness(LfColor color, float brightness) {
  uint32_t adjustedr = (int)(color.r * brightness);
  uint32_t adjustedg = (int)(color.g * brightness);
  uint32_t adjustedb = (int)(color.b * brightness);
  color.r = (unsigned char)(adjustedr > 255 ? 255 : adjustedr);
  color.g = (unsigned char)(adjustedg > 255 ? 255 : adjustedg);
  color.b = (unsigned char)(adjustedb > 255 ? 255 : adjustedb);
  return color; 
}

LfColor rn_color_alpha(LfColor color, uint8_t a) {
  return (LfColor){color.r, color.g, color.b, a};
}

vec4s rn_color_to_zto(LfColor color) {
  return (vec4s){color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
}

LfColor rn_color_from_hex(uint32_t hex) {
  LfColor color;
  color.r = (hex>> 16) & 0xFF;
  color.g = (hex >> 8) & 0xFF; 
  color.b = hex& 0xFF; 
  color.a = 255; 
  return color;
}

LfColor rn_color_from_zto(vec4s zto) {
  return (LfColor){(uint8_t)(zto.r * 255.0f), (uint8_t)(zto.g * 255.0f), (uint8_t)(zto.b * 255.0f), (uint8_t)(zto.a * 255.0f)};
}
