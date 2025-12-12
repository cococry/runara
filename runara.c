#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <float.h>
#include <fontconfig/fontconfig.h>
#include <cglm/mat4.h>
#include <cglm/types-struct.h>
#include <ctype.h>
#include "include/runara/runara.h"

#include "vendor/glad/include/glad/glad.h"
#include <math.h>
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image/stb_image.h"

#define LINESKY_IMPLEMENTATION 
#define LINESKY_STRIP_STRUCTURES
#include <linesky.h> 

#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


#ifdef _WIN32
#define HOMEDIR "USERPROFILE"
#else
#define HOMEDIR (char*)"HOME"
#endif

#define MAX(a, b) a > b ? a : b
#define MIN(a, b) a < b ? a : b

#define RN_TRACE(...) { printf("runara: [TRACE]: ");  printf(__VA_ARGS__); printf("\n"); } 
#define RN_INFO(...)  { printf("runara: [INFO]: ");   printf(__VA_ARGS__); printf("\n"); } 
#define RN_WARN(...)  { printf("runara: [WARN]: ");   printf(__VA_ARGS__); printf("\n"); } 
#define RN_ERROR(...) { fprintf(stderr, "runara: [ERROR]: ");  printf(__VA_ARGS__); printf("\n"); } 

static uint32_t         shader_create(GLenum type, const char* src);
static RnShader         shader_prg_create(const char* vert_src, const char* frag_src);
static void             shader_set_mat(RnShader prg, const char* name, mat4 mat); 
static void             set_projection_matrix(RnState* state);
static void             renderer_init(RnState* state);
static void             renderer_flush(RnState* state);
static void             renderer_begin(RnState* state);

static void             create_font_atlas(RnFont* font);


static RnGlyph*         get_glyph_from_codepoint(RnGlyphCache cache, RnFont font, uint64_t codepoint);
static RnGlyph          load_glyph_from_codepoint(RnFont* font, uint64_t codepoint, bool colored);
static RnGlyph          load_colr_glyph_from_codepoint(RnFont* font, uint64_t codepoint);
static RnGlyph          get_glyph_from_cache(RnGlyphCache* cache, RnFont* font, uint64_t codepoint);

static RnHarfbuzzText*  get_hb_text_from_str(RnHarfbuzzCache cache, RnFont font, const char* str);
static RnHarfbuzzText*  load_hb_text_from_str(RnFont font, const char* str);
static RnHarfbuzzText*  get_hb_text_from_cache(RnHarfbuzzCache* cache, RnFont font, const char* str);

static uint64_t         djb2_hash(const unsigned char *str);

// --- Static Functions ---



/* This function creates an OpenGL shader unit */
uint32_t 
shader_create(GLenum type, const char* src) {

  // Create & compile the shader source with OpenGL 
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

/* This function creates an OpenGL shader program consisting of 
 * a vertex- & fragment shader. 
 * */
RnShader 
shader_prg_create(const char* vert_src, const char* frag_src) {

  // Creating vertex & fragment shader with the shader API
  uint32_t vertex_shader = shader_create(GL_VERTEX_SHADER, vert_src);
  uint32_t fragment_shader = shader_create(GL_FRAGMENT_SHADER, frag_src);

  // Creating & linking the shader program with OpenGL
  RnShader prg;
  prg.id = glCreateProgram();
  glAttachShader(prg.id, vertex_shader);
  glAttachShader(prg.id, fragment_shader);
  glLinkProgram(prg.id);

  // Checking for linking errors
  int32_t linked;
  glGetProgramiv(prg.id, GL_LINK_STATUS, &linked);

  if(!linked) {
    RN_ERROR("Failed to link shader program.");
    char info[512];
    glGetProgramInfoLog(prg.id, 512, NULL, info);
    RN_INFO("%s", info);
    // Cleanup the shaders & the program
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

static uint32_t create_compute_program(const char* src) {
  GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
  glShaderSource(cs, 1, &src, NULL);
  glCompileShader(cs);
  GLint ok=0; glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
  if(!ok){ 
    GLint len=0; 
    glGetShaderiv(cs, GL_INFO_LOG_LENGTH, &len);
    char* log=(char*)malloc(len?len:1); 
    if(len) 
      glGetShaderInfoLog(cs,len,NULL,log); 
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, cs);
  glLinkProgram(prog);
  glDeleteShader(cs);
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if(!ok){ 
    GLint len=0; 
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    char* log=(char*)malloc(len?len:1); 
    if(len) 
      glGetProgramInfoLog(prog,len,NULL,log);
    fprintf(stderr,"[Compute] link error:\n%s\n", log); free(log);
  }
  return prog;
}

void 
shader_set_mat(RnShader prg, const char* name, mat4 mat) {
  glUniformMatrix4fv(glGetUniformLocation(prg.id, name), 1, GL_FALSE, mat[0]);
}

/* This function uploads the orthographic projection 
 * matrix that is used to crete the pixel space 
 * in which objects are rendered. 
 * */
void
set_projection_matrix(RnState* state) {
  mat4 orthoMatrix = GLM_MAT4_IDENTITY_INIT;
  glm_ortho(0.0f, (float)state->render.render_w,
            (float)state->render.render_h, 0.0f, 
            -1.0f, 1.0f,
            orthoMatrix);

  // Upload the matrix to the shader
  shader_set_mat(state->render.shader, "u_proj", orthoMatrix);
}

/* This function sets up OpenGL buffer object and shaders 
 * and sets up the state to use the batch rendering pipeline. 
 * */
void
renderer_init(RnState* state) {

  // OpenGL Setup 
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  // Allocate memory for vertices
  state->render.n_instances = 0;
  state->render.instances = (RnInstance*)calloc(RN_MAX_RENDER_BATCH, sizeof(RnInstance) );

  glGenVertexArrays(1, &state->render.vao);
  glBindVertexArray(state->render.vao);

  RnVertex quad_vertices[4] = {
    {{0.0f, 0.0f}, {0.0f, 0.0f}},
    {{1.0f, 0.0f}, {1.0f, 0.0f}},
    {{1.0f, 1.0f}, {1.0f, 1.0f}},
    {{0.0f, 1.0f}, {0.0f, 1.0f}},
  };
  uint32_t quad_indices[6] = {0, 1, 2, 2, 3, 0};

  glGenBuffers(1, &state->render.vbo_static);
  glBindBuffer(GL_ARRAY_BUFFER, state->render.vbo_static);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

  glGenBuffers(1, &state->render.ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state->render.ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STATIC_DRAW);

  // --- Vertex layout for static quad (binding 0) ---
  glEnableVertexAttribArray(0); // a_local_pos
  glVertexAttribPointer(
    0, 2, GL_FLOAT, GL_FALSE, sizeof(RnVertex), (void*)offsetof(RnVertex, pos));

  glEnableVertexAttribArray(1); // a_texcoord
  glVertexAttribPointer(
    1, 2, GL_FLOAT, GL_FALSE, sizeof(RnVertex), (void*)offsetof(RnVertex, texcoord));

  glGenBuffers(1, &state->render.vbo_instances);
  glBindBuffer(GL_ARRAY_BUFFER, state->render.vbo_instances);
  glBufferData(GL_ARRAY_BUFFER, sizeof(RnInstance) * RN_MAX_RENDER_BATCH, NULL, GL_DYNAMIC_DRAW); 

  GLsizei stride = sizeof(RnInstance);
  uintptr_t offset = 0;

  // i_pos : vec2
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offset);
  glVertexAttribDivisor(2, 1);
  offset += sizeof(float) * 2;

  // i_size : vec2
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offset);
  glVertexAttribDivisor(3, 1);
  offset += sizeof(float) * 2;

  // i_rotation : float
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offset);
  glVertexAttribDivisor(4, 1);
  offset += sizeof(float) * 1;

  // i_color : vec4 (u8 normalized)
  glEnableVertexAttribArray(5);
  glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offset);
  glVertexAttribDivisor(5, 1);
  offset += sizeof(uint8_t) * 4;

  // i_tex_index : uint (integer attribute)
  glEnableVertexAttribArray(6);
  glVertexAttribIPointer(6, 1, GL_UNSIGNED_BYTE, stride, (void*)offset);
  glVertexAttribDivisor(6, 1);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  /* Shader source code*/

  // Vertex shader
  const char* vert_src =
    "#version 460 core\n"
    "layout(location = 0) in vec2 a_local_pos;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "\n"
    "layout(location = 2) in vec2 i_pos;\n"
    "layout(location = 3) in vec2 i_size;\n"
    "layout(location = 4) in float i_rotation;\n"
    "layout(location = 5) in vec4 i_color;\n"
    "layout(location = 6) in int i_tex_index;\n"
    "\n"
    "uniform mat4 u_proj;\n"
    "\n"
    "out vec2 v_texcoord;\n"
    "out vec4 v_color;\n"
    "flat out int v_tex_index;\n"
    "\n"
    "void main()\n"
    "{\n"
    "    // Rotation 2x2\n"
    "    float c = cos(i_rotation);\n"
    "    float s = sin(i_rotation);\n"
    "    mat2 rot = mat2(c, -s, s, c);\n"
    "\n"
    "    // Transform\n"
    "    vec2 world = i_pos + rot * (a_local_pos * i_size);\n"
    "\n"
    "    v_texcoord = a_texcoord;\n"
    "    v_color = i_color;\n"
    "    v_tex_index = i_tex_index;\n"
    "    gl_Position = u_proj * vec4(world, 0.0, 1.0);\n"
    "}\n";


  const char* frag_src =
    "#version 460 core\n"
    "out vec4 o_color;\n"
    "\n"
    "in vec4 v_color;\n"
    "flat in int v_tex_index;\n"
    "in vec2 v_texcoord;\n"
    "\n"
    "uniform sampler2D u_textures[32];\n"
    "\n"
    "void main()\n"
    "{\n"
    "    vec4 col = v_color;\n"
    "    if (v_tex_index != 0) {\n"
    "        int idx = clamp(v_tex_index, 0, 31);\n"
    "        col *= texture(u_textures[idx - 1], v_texcoord);\n"
    "    }\n"
    "    o_color = col;\n"
    "}\n";

  // Creating the shader program with the source code of the 
  // vertex- and fragment shader
  state->render.shader = shader_prg_create(vert_src, frag_src);

  // initializing vertex position data
  state->render.vert_pos[0] = (vec4s){-0.5f, -0.5f, 0.0f, 1.0f};
  state->render.vert_pos[1] = (vec4s){0.5f, -0.5f, 0.0f, 1.0f};
  state->render.vert_pos[2] = (vec4s){0.5f, 0.5f, 0.0f, 1.0f};
  state->render.vert_pos[3] = (vec4s){-0.5f, 0.5f, 0.0f, 1.0f};

  // Populating the textures array in the shader with texture IDs 
  int32_t tex_slots[RN_MAX_TEX_COUNT_BATCH];
  for(uint32_t i = 0; i < RN_MAX_TEX_COUNT_BATCH; i++) {
    tex_slots[i] = i;
  }

  // Upload the texture array (sampler2D array) to the shader
  glUseProgram(state->render.shader.id);
  glBindVertexArray(state->render.vao);
  set_projection_matrix(state);
  glUniform1iv(glGetUniformLocation(state->render.shader.id, "u_textures"), RN_MAX_TEX_COUNT_BATCH, tex_slots);
}

/* This function renders every vertex in the current batch */
void 
renderer_flush(RnState* state) {
  if(state->render.n_instances <= 0) return;

  glBindBuffer(GL_ARRAY_BUFFER, state->render.vbo_instances);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(RnInstance) * state->render.n_instances, state->render.instances); 

  // Bind used texture slots
  for(uint32_t i = 0; i < state->render.tex_count; i++) {
    glBindTextureUnit(i, state->render.textures[i].id);
  }


  glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0, state->render.n_instances);
  state->drawcalls++;
}

/* This function begins a new batch within the 
 * renderer 
 * */
void renderer_begin(RnState* state) {
  // Resetting all the 
  state->render.n_instances = 0;
  state->render.tex_index = 0;
  state->render.tex_count = 0;
}

/* This function creates the atlas texture of 
 * a given font with OpenGL
 * */
void create_font_atlas(RnFont* font) {
  glGenTextures(1, &font->atlas_id);
  glBindTexture(GL_TEXTURE_2D, font->atlas_id);

  int32_t filter_mode = font->filter_mode == RN_TEX_FILTER_LINEAR ?
    GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter_mode);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter_mode); 

  glTexImage2D(
    GL_TEXTURE_2D,
    0, 
    GL_RGBA,
    font->atlas_w,
    font->atlas_h,
    0, 
    GL_RGBA,
    GL_UNSIGNED_BYTE,
    NULL);

  // Generate mipmaps
  glGenerateMipmap(GL_TEXTURE_2D);
}


RnGlyph* get_glyph_from_codepoint(RnGlyphCache cache, RnFont font, uint64_t codepoint) {
  for(uint32_t i = 0; i < cache.len; i++) {
    if(cache.data[i].codepoint == codepoint
      && cache.data[i].font_id == font.id) {
      return &cache.data[i];
    }
  }
  return NULL;
}


RnGlyph load_colr_glyph_from_codepoint(RnFont* font, uint64_t codepoint) {
  RnGlyph glyph = {0};

  FT_UInt glyph_index = codepoint; 

  if (FT_Load_Glyph(font->face, glyph_index, FT_LOAD_COLOR)) {
    RN_ERROR("Failed to load glyph index '%u'.", glyph_index);
    return glyph;
  }
  FT_GlyphSlot slot = font->face->glyph;
  if (slot->format == FT_GLYPH_FORMAT_BITMAP && slot->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
    return load_glyph_from_codepoint(font, codepoint, true);
  }

  FT_LayerIterator layer_iterator = {0};
  FT_UInt layer_glyph_index;
  FT_UInt layer_color_index;

  layer_iterator.p = NULL;

  FT_Bool has_layers = FT_Get_Color_Glyph_Layer(
    font->face,
    glyph_index,
    &layer_glyph_index,
    &layer_color_index,
    &layer_iterator
  );

  if (!has_layers) {
    return load_glyph_from_codepoint(font, codepoint, false);
  }

  // Select default palette (palette 0)
  FT_Color* palette = NULL;
  if (FT_Palette_Select(font->face, 0, &palette)) {
    palette = NULL; // fallback: no palette
  }

  int canvas_size = font->selected_strike_size;
  unsigned char* rgba_data = calloc(canvas_size * canvas_size * 4, 1);
  if (!rgba_data) {
    RN_ERROR("Failed to allocate RGBA canvas.");
    exit(EXIT_FAILURE);
  }

  int min_x = 9999, min_y = 9999;
  int max_x = -9999, max_y = -9999;

  FT_LayerIterator measure_iterator = {0};
  measure_iterator.p = NULL;
  FT_UInt measure_glyph_index, measure_color_index;

  // Get bounding box 
  if (FT_Get_Color_Glyph_Layer(font->face, glyph_index, &measure_glyph_index, &measure_color_index, &measure_iterator)) {
    do {
      if (FT_Load_Glyph(font->face, measure_glyph_index, FT_LOAD_RENDER)) {
        continue;
      }

      FT_GlyphSlot slot = font->face->glyph;
      if (slot->format != FT_GLYPH_FORMAT_BITMAP)
        continue;

      int glyph_min_x = slot->bitmap_left;
      int glyph_min_y = -slot->bitmap_top + slot->bitmap.rows;
      int glyph_max_x = glyph_min_x + slot->bitmap.width;
      int glyph_max_y = glyph_min_y + slot->bitmap.rows;

      if (glyph_min_x < min_x) min_x = glyph_min_x;
      if (glyph_min_y < min_y) min_y = glyph_min_y;
      if (glyph_max_x > max_x) max_x = glyph_max_x;
      if (glyph_max_y > max_y) max_y = glyph_max_y;

    } while (FT_Get_Color_Glyph_Layer(font->face, glyph_index, &measure_glyph_index, &measure_color_index, &measure_iterator));
  }

  if (min_x > max_x || min_y > max_y) {
    free(rgba_data);
    RN_ERROR("Invalid bounding box for COLR glyph.");
    return glyph;
  }

  int glyph_width = max_x - min_x;
  int glyph_height = max_y - min_y;

  // Composite layers
  layer_iterator.p = NULL;
  if (FT_Get_Color_Glyph_Layer(font->face, glyph_index, &layer_glyph_index, &layer_color_index, &layer_iterator)) {
    do {
      if (FT_Load_Glyph(font->face, layer_glyph_index, FT_LOAD_RENDER)) {
        continue;
      }

      FT_GlyphSlot slot = font->face->glyph;
      if (slot->format != FT_GLYPH_FORMAT_BITMAP)
        continue;

      FT_Color layer_color;
      if (layer_color_index == 0xFFFF || !palette) {
        layer_color.red   = 0x00;
        layer_color.green = 0x00;
        layer_color.blue  = 0x00;
        layer_color.alpha = 0xFF;
      } else {
        layer_color = palette[layer_color_index];
      }

      for (uint32_t y = 0; y < slot->bitmap.rows; y++) {
        for (uint32_t x = 0; x < slot->bitmap.width; x++) {
          unsigned char coverage = slot->bitmap.buffer[y * slot->bitmap.pitch + x];
          if (coverage == 0)
            continue;

          int dst_x = (slot->bitmap_left + x) - min_x;
          int dst_y = (glyph_height - (slot->bitmap_top - y)) - min_y;

          if (dst_x < 0 || dst_x >= canvas_size || dst_y < 0 || dst_y >= canvas_size)
            continue;

          unsigned char* pixel = &rgba_data[(dst_y * canvas_size + dst_x) * 4];

          unsigned char src_r = (layer_color.red   * coverage) >> 8;
          unsigned char src_g = (layer_color.green * coverage) >> 8;
          unsigned char src_b = (layer_color.blue  * coverage) >> 8;
          unsigned char src_a = (layer_color.alpha * coverage) >> 8;

          pixel[0] = src_r;
          pixel[1] = src_g;
          pixel[2] = src_b;
          pixel[3] = src_a;
        }
      }

    } while (FT_Get_Color_Glyph_Layer(font->face, glyph_index, &layer_glyph_index, &layer_color_index, &layer_iterator));
  }

  glBindTexture(GL_TEXTURE_2D, font->atlas_id);

  if (font->atlas_x + glyph_width >= font->atlas_w) {
    font->atlas_x = 0;
    font->atlas_y += font->atlas_row_h;
    font->atlas_row_h = 0;
  }

  if (font->atlas_y + glyph_height >= font->atlas_h) {
    RN_ERROR("Font atlas overflow (vertical). Not handled yet.");
    free(rgba_data);
    return glyph;
  }

  glTexSubImage2D(
    GL_TEXTURE_2D,
    0,
    font->atlas_x,
    font->atlas_y,
    glyph_width,
    glyph_height,
    GL_RGBA,
    GL_UNSIGNED_BYTE,
    rgba_data
  );

  glGenerateMipmap(GL_TEXTURE_2D);

  float scale = 1.0f;
  if (font->selected_strike_size)
    scale = ((float)font->size / (float)font->selected_strike_size);

  // Unscaled UVs (stay relative to atlas)
  glyph.u0 = (float)font->atlas_x / (float)font->atlas_w;
  glyph.v0 = (float)font->atlas_y / (float)font->atlas_h;
  glyph.u1 = (float)(font->atlas_x + glyph_width) / (float)font->atlas_w;
  glyph.v1 = (float)(font->atlas_y + glyph_height - 1) / (float)font->atlas_h;

  glyph.width     = glyph_width * scale;
  glyph.height    = glyph_height * scale;
  glyph.glyph_top = (float)slot->bitmap_top;
  glyph.glyph_bottom = (float)((int)slot->bitmap_top - (int)slot->bitmap.rows);
  glyph.bearing_x = min_x * scale;
  glyph.bearing_y = -min_y * scale;
  glyph.advance   = (font->face->glyph->advance.x / 64.0f) * scale; // remember divide by 64!

  glyph.font_id = font->id;
  glyph.codepoint = codepoint;

  font->atlas_x += glyph_width + 1;
  font->atlas_row_h = (font->atlas_row_h > (uint32_t)glyph_height) ? font->atlas_row_h : (uint32_t)glyph_height;

  // Cleanup
  free(rgba_data);

  // Return the glyph
  return glyph;
}

/* This function loads a glyph's bitmap from a given glyph index from a font.
* The bitmap is uploade to the texture atlas of the font. If the glyph does 
* not fit onto the atlas, the atlas texture is resize. Glyphs are padded 
* within the atlas 
* */
RnGlyph 
load_glyph_from_codepoint(RnFont* font, uint64_t codepoint, bool colored) {
  RnGlyph glyph;
  // Load the glyph with freetype
  uint32_t flags = colored ? FT_LOAD_RENDER | FT_LOAD_COLOR : FT_LOAD_RENDER;
  if (FT_Load_Glyph(font->face, codepoint, flags)) {
    RN_ERROR("Failed to load glyph of character with codepoint '%lu'.", codepoint);
    return glyph;
  }

  // Retrieving glyph information 
  FT_GlyphSlot slot = font->face->glyph;
  int32_t width, height;

  int bpp = 4; 
  int padding = 1; 

  int old_width = slot->bitmap.width;
  int old_height = slot->bitmap.rows;

  width = old_width + padding * 2.0f;
  height = old_height + padding * 2.0f; 

  // Allocate memory for RGBA data with padding
  unsigned char* rgba_data = (unsigned char*)malloc(width * height * bpp);
  if (rgba_data == NULL) {
    fprintf(stderr, "Memory allocation failed\n");
    exit(EXIT_FAILURE);
  }

  // Initialize the buffer with transparent color (RGBA = 0, 0, 0, 0)
  memset(rgba_data, 0, width * height * bpp);

  if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY || !colored) {
    // Grayscale glyph (normal text)
    for (int y = 0; y < old_height; y++) {
      for (int x = 0; x < old_width; x++) {
        unsigned char* src_pixel = &slot->bitmap.buffer[y * slot->bitmap.pitch + x];
        unsigned char* dst_pixel = &rgba_data[((y + padding) * width + (x + padding)) * bpp];
        unsigned char gray = *src_pixel;

        dst_pixel[0] = gray;    // R
        dst_pixel[1] = gray;    // G
        dst_pixel[2] = gray;    // B
        dst_pixel[3] = gray;   // A (coverage)
      }
    }
  }
  else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
    // Color bitmap glyph (emoji)
    for (int y = 0; y < old_height; y++) {
      for (int x = 0; x < old_width; x++) {
        unsigned char* src_pixel = &slot->bitmap.buffer[(y * slot->bitmap.pitch) + (x * 4)];
        unsigned char* dst_pixel = &rgba_data[((y + padding) * width + (x + padding)) * bpp];

        dst_pixel[0] = src_pixel[2]; // R
        dst_pixel[1] = src_pixel[1]; // G
        dst_pixel[2] = src_pixel[0]; // B
        dst_pixel[3] = src_pixel[3]; // A
      }
    }
  }
  else {
    RN_ERROR("Unsupported pixel mode: %d", slot->bitmap.pixel_mode);
  }

  // When the atlas overflows on the X, advance 
  // one line down
  if (font->atlas_x + width > font->atlas_w) {
    font->atlas_x = 0;
    font->atlas_y += font->atlas_row_h;
    font->atlas_row_h = 0;
  }

  // Resize the atlas if it overflows on the Y
  if (font->atlas_y + height > font->atlas_h) {
    int new_w = font->atlas_w * 2;
    int new_h = font->atlas_h * 2 + 1;

    uint32_t new_id;
    // Create new texture
    glGenTextures(1, &new_id);
    glBindTexture(GL_TEXTURE_2D, new_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, new_w, new_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    // Copy old texture data
    glBindTexture(GL_TEXTURE_2D, font->atlas_id);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, font->atlas_w, font->atlas_h);

    // Delete old texture
    glDeleteTextures(1, &font->atlas_id);

    // Set new values
    font->atlas_id = new_id;
    font->atlas_w = new_w;
    font->atlas_h = new_h;

    // Bind the new texture
    glBindTexture(GL_TEXTURE_2D, font->atlas_id);

    // Initialize new texture data
    glTexImage2D(
      GL_TEXTURE_2D,
      0, 
      GL_RGBA, 
      font->atlas_w, 
      font->atlas_h,
      0,
      GL_RGBA,
      GL_UNSIGNED_BYTE,
      NULL);
  }

  // Bind the texture atlas
  glBindTexture(GL_TEXTURE_2D, font->atlas_id);

  // Set texture attributes
  int32_t filter_mode = font->filter_mode == RN_TEX_FILTER_LINEAR ?
    GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter_mode);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter_mode);

  // Upload the glyph's bitmap to the atlas
  glTexSubImage2D(
    GL_TEXTURE_2D, 
    0, 
    font->atlas_x, 
    font->atlas_y, 
    width, 
    height,
    GL_RGBA,
    GL_UNSIGNED_BYTE, 
    rgba_data);

  glGenerateMipmap(GL_TEXTURE_2D);
  /* Set glyph attributes */

  float scale = 1.0f;
  if (font->selected_strike_size)
    scale = ((float)font->size / (float)font->selected_strike_size);

  glyph.width     = slot->bitmap.width * scale;
  glyph.height    = slot->bitmap.rows * scale;
  glyph.glyph_top = (float)slot->bitmap_top;
  glyph.glyph_bottom = (float)((int)slot->bitmap_top - (int)slot->bitmap.rows);
  glyph.bearing_x = slot->bitmap_left * scale;
  glyph.bearing_y = slot->bitmap_top * scale;
  glyph.advance   = (slot->advance.x / 64.0f) * scale;
  glyph.ascender  = (slot->metrics.horiBearingY >> 6) * scale;
  glyph.descender = ((slot->metrics.horiBearingY - slot->metrics.height) / 64.0f) * scale;

  glyph.codepoint = codepoint;
  glyph.font_id = font->id;

  glyph.u0 = (float)(font->atlas_x + padding) / (float)font->atlas_w;
  glyph.v0 = (float)(font->atlas_y + padding) / (float)font->atlas_h;
  glyph.u1 = (float)(font->atlas_x + width)   / (float)font->atlas_w;
  glyph.v1 = (float)(font->atlas_y + height) / (float)font->atlas_h;

  font->atlas_x += width + 1;
  font->atlas_row_h = (font->atlas_row_h > height) ? font->atlas_row_h : height;

  // Cleanup
  free(rgba_data);

  // Return final glyph
  return glyph;


  // Free allocated memory
  free(rgba_data);

  return glyph;
}

RnGlyph get_glyph_from_cache(RnGlyphCache* cache, RnFont* font, uint64_t codepoint) {
  RnGlyph* glyph = get_glyph_from_codepoint(*cache, *font, codepoint);

  if(glyph) {
    return *glyph;
  }

  RnGlyph new_glyph = load_colr_glyph_from_codepoint(font, codepoint);
  DA_PUSH(cache, new_glyph);
  return new_glyph; 
}

RnHarfbuzzText* get_hb_text_from_str(RnHarfbuzzCache cache, RnFont font, const char* str) {
  uint64_t hash = djb2_hash((unsigned char*)str);
  for(uint32_t i = 0; i < cache.len; i++) {
    if(cache.data[i]->hash == hash && 
      cache.data[i]->font_id == font.id) {
      return cache.data[i];
    }
  }
  return NULL;
}

/*
 * This function loads the 
 * text rendering information for a given string 
 * with harfbuzz */
RnHarfbuzzText*
load_hb_text_from_str(RnFont font, const char* str) {
  RnHarfbuzzText* text = malloc(sizeof(*text));
  text->words  = NULL;
  text->nwords = 0;

  // Create a HarfBuzz buffer and add text
  text->buf = hb_buffer_create();
  hb_buffer_add_utf8(text->buf, str, -1, 0, -1);

  // Shape the text
  hb_buffer_guess_segment_properties(text->buf);
  hb_shape(font.hb_font, text->buf, NULL, 0);

  int32_t len;
  // Retrieve glyph information and positions
  text->glyph_info = hb_buffer_get_glyph_infos(text->buf, &text->glyph_count);
  text->glyph_pos = hb_buffer_get_glyph_positions(text->buf, &text->glyph_count);

  // Generate a hash for the text
  text->hash = djb2_hash((const unsigned char*)str);

  // Set font ID for the harfbuzz text
  text->font_id = font.id;

  // Set rendered string of the text 
  text->str = malloc(strlen(str) + 1);
  strcpy(text->str, str);

  text->highest_bearing = 0.0f;

  return text;
}

RnHarfbuzzText* get_hb_text_from_cache(RnHarfbuzzCache* cache, RnFont font, const char* str) {
  RnHarfbuzzText* text = get_hb_text_from_str(*cache, font, str);

  if(text) {
    return text;
  }

  RnHarfbuzzText* new_text = load_hb_text_from_str(font, str);
  DA_PUSH(cache, new_text);
  return new_text; 
}

/*
 * Returns the DJB2 hash of a given string
 * */
uint64_t 
djb2_hash(const unsigned char *str) {
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
RnState*
rn_init(uint32_t render_w, uint32_t render_h, RnGLLoader loader) {
  RnState* state = malloc(sizeof(*state));

  // Set locale to ensure that unicode is working
  setlocale(LC_ALL, "");

  // Load OpenGL functions with glad
  if(loader && !gladLoadGLLoader((GLADloadproc)loader)) {
    RN_ERROR("Failed to initialize Glad.");
    return state;
  }

  // Set default state
  state->render.render_w = render_w;
  state->render.render_h = render_h;
  state->render.tex_count = 0;
  state->drawcalls = 0;

  state->cull_start = (vec2s){-1, -1};
  state->cull_end = (vec2s){-1, -1};

  // Initializing the renderer
  renderer_init(state);

  // Initializing FreeType
  if(FT_Init_FreeType(&state->ft) != 0) {
    RN_ERROR("Failed to initialize FreeType.");
    return state;
  }

  state->glyph_cache = (RnGlyphCache)DA_INIT;
  state->hb_cache = (RnHarfbuzzCache)DA_INIT;

  state->init = true;

  return state;
}

void 
rn_terminate(RnState* state) {
  // Free glyph- & harfbuzz-caches
  DA_FREE(&state->glyph_cache);
  DA_FREE(&state->hb_cache);

  // Terminate freetype
  FT_Done_FreeType(state->ft);

  free(state);
}

void
rn_resize_display(RnState* state, uint32_t render_w, uint32_t render_h) {
  // Set render dimensions
  state->render.render_w = render_w;
  state->render.render_h = render_h;

  // Send the dimension chnage to OpenGL 
  glViewport(0, 0, render_w, render_h);
  set_projection_matrix(state);
}

RnTexture 
rn_load_texture(const char* filepath) {
  return rn_load_texture_ex(filepath, false, RN_TEX_FILTER_LINEAR);
}

void 
rn_load_texture_base_types(
  const char* filepath, 
  uint32_t* o_tex_id, 
  uint32_t* o_tex_width, 
  uint32_t* o_tex_height,
  uint32_t filter) {
  int width, height, channels;

  // Load image data with stb_image
  unsigned char* image = stbi_load(filepath, &width, &height, &channels, STBI_rgb_alpha);
  if (!image) {
    RN_ERROR("Failed to load texture at '%s'.", filepath);
    return;
  }

  // Create OpenGL texture 
  glGenTextures(1, o_tex_id);
  glBindTexture(GL_TEXTURE_2D, *o_tex_id); 

  // Set texture parameters
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  switch(filter) {
    case RN_TEX_FILTER_LINEAR:
      glTextureParameteri(*o_tex_id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTextureParameteri(*o_tex_id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      break;
    case RN_TEX_FILTER_NEAREST:
      glTextureParameteri(*o_tex_id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTextureParameteri(*o_tex_id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      break;
  }

  // Load texture data
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image);
  glGenerateMipmap(GL_TEXTURE_2D);
  // Free image data CPU side 
  stbi_image_free(image); 

  *o_tex_width = width;
  *o_tex_height = height;

}

RnTexture
rn_load_texture_ex(const char* filepath, bool flip, RnTextureFiltering filter) {
  RnTexture tex;
  int width, height, channels;

  stbi_set_flip_vertically_on_load(flip);
  // Load image data with stb_image
  unsigned char* image = stbi_load(filepath, &width, &height, &channels, STBI_rgb_alpha);
  if (!image) {
    RN_ERROR("Failed to load texture at '%s'.", filepath);
    return tex;
  }

  // Create OpenGL texture 
  glGenTextures(1, &tex.id);
  glBindTexture(GL_TEXTURE_2D, tex.id); 

  // Set texture parameters
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  switch(filter) {
    case RN_TEX_FILTER_LINEAR:
      glTextureParameteri(tex.id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTextureParameteri(tex.id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      break;
    case RN_TEX_FILTER_NEAREST:
      glTextureParameteri(tex.id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTextureParameteri(tex.id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      break;
  }

  // Load texture data
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image);
  glGenerateMipmap(GL_TEXTURE_2D);
  // Free image data CPU side 
  stbi_image_free(image); 

  // Set texture dimensions
  tex.width = width;
  tex.height = height;
  return tex;
}

RnFont* rn_load_font_ex(RnState* state, const char* filepath, uint32_t size,
                        uint32_t atlas_w, uint32_t atlas_h, uint32_t tab_w,
                        RnTextureFiltering filter_mode, uint32_t face_idx) {
  RnFont* font = malloc(sizeof(*font));
  FT_Face face;

  if(!size) return NULL;

  // Create a new face from the filepath with freetype
  if(FT_New_Face(state->ft, filepath, face_idx, &face)) {
    RN_ERROR("Failed to load font file '%s'.", filepath);
    return NULL;
  }
  if (FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
    // fallback: find any Unicode-compatible charmap
    for (int i = 0; i < face->num_charmaps; i++) {
      if (face->charmaps[i]->encoding == FT_ENCODING_UNICODE) {
        FT_Set_Charmap(face, face->charmaps[i]);
        break;
      }
    }
  }

  if (face->num_fixed_sizes > 0) {
    // Select the closest strike available
    int best_match = 0;
    int best_diff = abs((int32_t)(face->available_sizes[0].height - size));

    for (int i = 1; i < face->num_fixed_sizes; i++) {
      int diff = abs((int32_t)(face->available_sizes[i].height - size));
      if (diff < best_diff) {
        best_match = i;
        best_diff = diff;
      }
    }

    if (FT_Select_Size(face, best_match)) {
      RN_ERROR("Failed to select bitmap strike.");
      return NULL;
    }
    font->selected_strike_size = face->available_sizes[best_match].height;
  } else {
    // No fixed sizes (normal vector font), fallback to pixel size
    FT_Set_Pixel_Sizes(face, 0, size);
    font->selected_strike_size = 0; 
  }
  font->face = face;
  font->size = size;

  // Create the harfbuzz font handle
  font->hb_font = hb_ft_font_create(font->face, NULL);

  font->id = state->font_id++;

  font->atlas_w = atlas_w;
  font->atlas_h = atlas_h;
  font->atlas_row_h = 0;
  font->atlas_x = 0;
  font->atlas_y = 0;
  font->filepath = strdup(filepath);
  font->face_idx = face_idx;

  font->tab_w = tab_w;

  font->filter_mode = filter_mode;

  // Create the OpenGL font atlas texture 
  create_font_atlas(font);

  // Get the width of the space character within the 
  // font to know how wide tab character should be.
  if (FT_Load_Char(font->face, ' ', FT_LOAD_DEFAULT) != 0) {
    return font;
  }

  FT_GlyphSlot slot = face->glyph;
  font->space_w = rn_text_props(state, " ", font).width;

  font->line_h = font->face->size->metrics.height / 64.0f;

  return font;
}

RnFont* 
rn_create_font_from_loaded_data_ex(RnState* state, FT_Face face, hb_font_t* hb_font,
                                   uint32_t size, uint32_t atlas_w, uint32_t atlas_h, 
                                   uint32_t tab_w, RnTextureFiltering filter_mode, 
                                   uint32_t face_idx, const char* filepath, float space_w) {
  RnFont* font = malloc(sizeof(*font));

  if (!font) {
    RN_ERROR("Failed to allocate memory for font.");
    return NULL;
  }

  font->face = face;
  font->size = size;

  // Use the provided Harfbuzz font handle
  font->hb_font = hb_font;

  font->id = state->font_id++;

  font->atlas_w = atlas_w;
  font->atlas_h = atlas_h;
  font->atlas_row_h = 0;
  font->atlas_x = 0;
  font->atlas_y = 0;

  font->filepath = strdup(filepath);
  font->face_idx = face_idx;
  font->tab_w = tab_w;
  font->filter_mode = filter_mode;

  // Create the OpenGL font atlas texture 
  create_font_atlas(font);

  // Use the provided space_w instead of calculating it
  font->space_w = space_w;

  font->line_h = font->face->size->metrics.height / 64.0f;

  return font;
}

RnFont* 
rn_create_font_from_loaded_data(RnState* state, FT_Face face, hb_font_t* hb_font, float space_w,
                                uint32_t size, 
                                uint32_t face_idx, const char* filepath) {
  return rn_create_font_from_loaded_data_ex(state, face, hb_font, size, 1024, 1024, 4, RN_TEX_FILTER_LINEAR, face_idx, filepath, space_w);
}

RnFont* 
rn_load_font(RnState* state, const char* filepath, uint32_t size) {
  return rn_load_font_ex(state, filepath, size, 
                         1024, 1024, 4, RN_TEX_FILTER_LINEAR, 0);
}

RnFont* 
rn_load_font_from_face(RnState* state, const char* filepath, uint32_t size, uint32_t face_idx) {
  return rn_load_font_ex(state, filepath, size, 
                         1024, 1024, 4, RN_TEX_FILTER_LINEAR, face_idx);
}

void 
rn_set_font_size(RnState* state, RnFont* font, uint32_t size) {
  if(font->size == size) return;

  // Set size of the font
  font->size = size;

  // Reset the font size
  FT_Set_Pixel_Sizes(font->face, 0, size);

  // Reload the harfbuzz font
  hb_font_destroy(font->hb_font);
  font->hb_font = hb_ft_font_create(font->face, NULL);

  // Reload the glyph & harfbuzz cache
  rn_reload_font_harfbuzz_cache(state, *font);
  rn_reload_font_glyph_cache(state, font);

  font->space_w = rn_text_props(state, " ", font).width;
  font->line_h = font->face->size->metrics.height / 64.0f;
} 

void
rn_free_texture(RnTexture* tex) {
  // Delete the OpenGL texture
  glDeleteTextures(1, &tex->id);
  // Zero-out the texture handle
  memset(tex, 0, sizeof(*tex));
}

void
rn_free_font(RnState* state, RnFont* font) {
  (void)state;
  // Cleanup the freetype font handle
  FT_Done_Face(font->face);
  // Destroy the harfbuzz font handle
  hb_font_destroy(font->hb_font);

  // Delete the font's atlas texture
  glDeleteTextures(1, &font->atlas_id);

  free(font);
}

void 
rn_clear_color(RnColor color) {
  vec4s zto = rn_color_to_zto(color);
  glClearColor(zto.r, zto.g, zto.b, zto.a);
  glClear(GL_COLOR_BUFFER_BIT);
}

void 
rn_begin_scissor(vec2s pos, vec2s size, uint32_t render_height) {
  int32_t y_lower_left = render_height - (pos.y + size.y);
  glEnable(GL_SCISSOR_TEST);
  glScissor(pos.x, y_lower_left, size.x, size.y);
}
void 
rn_end_scissor(void) {
  glDisable(GL_SCISSOR_TEST);
}

void 
rn_clear_color_base_types(
  unsigned char r, 
  unsigned char g, 
  unsigned char b, 
  unsigned char a) {
  RnColor color = (RnColor){r, g, b, a};
  vec4s zto = rn_color_to_zto(color);
  glClearColor(zto.r, zto.g, zto.b, zto.a);
  glClear(GL_COLOR_BUFFER_BIT);
}

void
rn_begin_batch(RnState* state) {
  renderer_begin(state);
  state->drawcalls = 0;
}

void rn_begin(RnState* state) {
  rn_begin_batch(state); 
}

void
rn_next_batch(RnState* state) {
  // End the current batch
  renderer_flush(state);
  // Begin a new batch
  renderer_begin(state);
}

RnInstance* rn_add_instance(RnState* state,
    vec2s pos, vec2s size, float rotation, RnColor color,
    uint8_t tex_index) {
  if(state->render.n_instances  + 1 >= RN_MAX_RENDER_BATCH) {
    renderer_flush(state);
    state->render.n_instances = 0;
  }
  RnInstance* inst = &state->render.instances[state->render.n_instances++];

  inst->size[0] = size.x; inst->size[1] = size.y;
  
  inst->pos[0] = pos.x; inst->pos[1] = pos.y;

  inst->color[0] = color.r;
  inst->color[1] = color.g;
  inst->color[2] = color.b;
  inst->color[3] = color.a;

  inst->tex_index = tex_index;

  inst->rotation = rotation;

  return inst;

}


uint8_t rn_tex_index_from_tex(RnState* state, RnTexture tex) {
  uint8_t tex_index = 0; 
  for (uint32_t i = 0; i < state->render.tex_count; ++i) {
    if (tex.id == state->render.textures[i].id) {
      tex_index = i + 1;
      break;
    }
  }
  return tex_index;
}

void
rn_add_tex_to_batch(RnState* state, RnTexture tex) {
  state->render.textures[state->render.tex_count++] = tex;
  state->render.tex_index++;
}

void
rn_end_batch(RnState* state) {
  renderer_flush(state);
}

void 
rn_end(RnState* state) {
  rn_end_batch(state);
}

void
rn_reload_font_glyph_cache(RnState* state, RnFont* font) {
  RnGlyph* glyph = NULL, *tmp = NULL;

  font->atlas_w = 1024;
  font->atlas_h = 1024;
  font->atlas_row_h = 0;
  font->atlas_x = 0;
  font->atlas_y = 0;

  glDeleteTextures(1, &font->atlas_id);
  create_font_atlas(font);

  for(uint32_t i = 0; i < state->glyph_cache.len; i++) {
    RnGlyph* glyph = &state->glyph_cache.data[i];
    if(glyph->font_id == font->id) {
      *glyph = load_colr_glyph_from_codepoint(font, glyph->codepoint);
    }
  }
}

void 
rn_reload_font_harfbuzz_cache(RnState* state, RnFont font) {
  RnHarfbuzzText* text = NULL, *tmp = NULL;
  for(uint32_t i = 0; i < state->hb_cache.len; i++) {
    RnHarfbuzzText* text = state->hb_cache.data[i];
    if(text->font_id == font.id) {
      hb_buffer_destroy(text->buf);
      char* tmp_str = strdup(text->str);
      free(text);
      text = load_hb_text_from_str(font, tmp_str); 
      free(tmp_str);
    }
  }
}

void
rn_rect_render_ex(
  RnState* state, 
  vec2s pos, 
  vec2s size, 
  float rotation_angle,
  RnColor color, 
  RnColor border_color, 
  float border_width,
  float corner_radius) {
  rn_add_instance(state, pos, size, rotation_angle, color,0); 
}

void rn_rect_render(
  RnState* state, 
  vec2s pos, 
  vec2s size, 
  RnColor color) {
  rn_rect_render_ex(state, pos, size, 0.0f, color, 
                           RN_NO_COLOR, 0.0f, 0.0f);
}

void rn_rect_render_base_types(
  RnState* state, 
  float posx,
  float posy,
  float width,
  float height,
  float rotation_angle,
  unsigned char color_r,
  unsigned char color_g,
  unsigned char color_b,
  unsigned char color_a) {
  rn_rect_render_ex(state, (vec2s){posx, posy}, 
                           (vec2s){width, height}, rotation_angle, 
                           (RnColor){color_r, color_g, color_b, color_a}, 
                           RN_NO_COLOR, 0.0f, 0.0f);
}

void rn_image_render_adv(
  RnState* state, 
  vec2s pos, 
  float rotation_angle,
  RnColor color, 
  RnTexture tex,
  vec2s* texcoords,
  bool is_text,
  RnColor border_color,
  float border_width, 
  float corner_radius) {
  // Find or add texture and get it's index
  uint8_t tex_index = rn_tex_index_from_tex(state, tex);

  if (tex_index == 0) {
    rn_add_tex_to_batch(state, tex);
    tex_index = (float)state->render.tex_count;
  }

  rn_add_instance(state, pos, (vec2s){tex.width, tex.height}, 0.0f, color, tex_index);
}

void rn_image_render_ex(
  RnState* state, 
  vec2s pos, 
  float rotation_angle,
  RnColor color, 
  RnTexture tex,
  RnColor border_color,
  float border_width, 
  float corner_radius) {

  vec2s texcoords[4] = { 
    (vec2s){0.0f, 0.0f}, 
    (vec2s){1.0f, 0.0f}, 
    (vec2s){1.0f, 1.0f}, 
    (vec2s){0.0f, 1.0f} 
  };
  rn_image_render_adv(state, pos, rotation_angle, color, 
                      tex, texcoords, false,
                      border_color, border_width,
                      corner_radius);
}

void rn_image_render(
  RnState* state,
  vec2s pos, 
  RnColor color, 
  RnTexture tex) {
  rn_image_render_ex(state, pos, 0.0f, color, tex,
                     RN_NO_COLOR, 0.0f, 0.0f);
}

void rn_image_render_base_types(
  RnState* state, 
  float posx, 
  float posy, 
  float rotation_angle,
  unsigned char color_r, 
  unsigned char color_g, 
  unsigned char color_b, 
  unsigned char color_a, 
  uint32_t tex_id, uint32_t tex_width, uint32_t tex_height) {

  rn_image_render_ex(state, (vec2s){posx, posy}, rotation_angle, 
                     (RnColor){color_r, color_g, color_b, color_a},
                     (RnTexture){.id = tex_id, .width = tex_width, .height = tex_height},
                     RN_NO_COLOR,0.0f,0.0f);
}


uint32_t rn_utf8_to_codepoint(const char *text, uint32_t cluster, uint32_t text_length) {
  uint32_t codepoint = 0;
  uint8_t c = text[cluster];

  if (c < 0x80) {
    // 1-byte UTF-8
    codepoint = c;
  } else if ((c & 0xE0) == 0xC0 && (cluster + 1) < text_length) {
    // 2-byte UTF-8
    codepoint = ((c & 0x1F) << 6) | (text[cluster + 1] & 0x3F);
  } else if ((c & 0xF0) == 0xE0 && (cluster + 2) < text_length) {
    // 3-byte UTF-8
    codepoint = ((c & 0x0F) << 12) |
      ((text[cluster + 1] & 0x3F) << 6) |
      (text[cluster + 2] & 0x3F);
  } else if ((c & 0xF8) == 0xF0 && (cluster + 3) < text_length) {
    // 4-byte UTF-8
    codepoint = ((c & 0x07) << 18) |
      ((text[cluster + 1] & 0x3F) << 12) |
      ((text[cluster + 2] & 0x3F) << 6) |
      (text[cluster + 3] & 0x3F);
  }

  return codepoint;
}


RnTextProps rn_text_render_ex(RnState* state, 
                              const char* text, 
                              RnFont* font, 
                              vec2s pos, 
                              RnColor color, 
                              float line_height,
                              bool render) {

  // Get the harfbuzz text information for the string
  RnHarfbuzzText* hb_text = rn_hb_text_from_str(state, *font, text);

  // Retrieve highest bearing if 
  // it was not retrived yet.
  if(!hb_text->highest_bearing) {
    for (unsigned int i = 0; i < hb_text->glyph_count; i++) {
      // Get the glyph from the glyph index 
      RnGlyph glyph =  rn_glyph_from_codepoint(
        state, font,
        hb_text->glyph_info[i].codepoint);
      // Check if the glyph's bearing is higher 
      // than the current highest bearing
      if(glyph.bearing_y > hb_text->highest_bearing) {
        hb_text->highest_bearing = glyph.bearing_y;
      }
    }
  }

  vec2s start_pos = (vec2s){.x = pos.x, .y = pos.y};

  // New line characters
  const int32_t line_feed       = 0x000A;
  const int32_t carriage_return = 0x000D;
  const int32_t line_seperator  = 0x2028;
  const int32_t paragraph_seperator = 0x2029;

  float textheight = 0;

  float scale = 1.0f;
  if (font->selected_strike_size)
    scale = ((float)font->size / (float)font->selected_strike_size);
  for (unsigned int i = 0; i < hb_text->glyph_count; i++) {
    // Get the glyph from the glyph index
    RnGlyph glyph =  rn_glyph_from_codepoint(
      state, font,
      hb_text->glyph_info[i].codepoint); 

    uint32_t text_length = strlen(text);
    uint32_t codepoint = rn_utf8_to_codepoint(text, hb_text->glyph_info[i].cluster, text_length);
    // Check if the unicode codepoint is a new line and advance 
    // to the next line if so
    if(codepoint == line_feed || codepoint == carriage_return ||
      codepoint == line_seperator || codepoint == paragraph_seperator) {
      float font_height = font->face->size->metrics.height / 64.0f;
      pos.x = start_pos.x;
      pos.y += line_height ? line_height : font_height;
      textheight += line_height ? line_height : font_height;
      continue;
    }

    // Advance the x position by the tab width if 
    // we iterate a tab character
    if(codepoint == '\t') {
      pos.x += font->tab_w * font->space_w;
      continue;
    }

    // If the glyph is not within the font, dont render it
    if(!hb_text->glyph_info[i].codepoint) {
      continue;
    }
    float x_advance = (hb_text->glyph_pos[i].x_advance / 64.0f) * scale;
    float y_advance = (hb_text->glyph_pos[i].y_advance / 64.0f) * scale;
    float x_offset  = (hb_text->glyph_pos[i].x_offset / 64.0f) * scale;
    float y_offset  = (hb_text->glyph_pos[i].y_offset / 64.0f) * scale;


    vec2s glyph_pos = {
      pos.x + x_offset,
      pos.y + hb_text->highest_bearing - y_offset 
    };

    // Render the glyph
    if(render) {
      rn_glyph_render(state, glyph, *font, glyph_pos, color);
    }

    if(glyph.height > textheight) {
      textheight = glyph.height;
    }

    // Advance to the next glyph
    pos.x += x_advance; 
    pos.y += y_advance;
  }

  return (RnTextProps){
    .width = pos.x - start_pos.x, 
    .height = textheight,
    .paragraph_pos = pos
  };
}


char* trimspaces(char* str) {
  char* end;

  // Trim leading space
  while (isspace((unsigned char)*str)) str++;

  if (*str == 0)  // All spaces?
    return str;

  // Trim trailing space
  end = str + strlen(str) - 1;
  while (end > str && isspace((unsigned char)*end)) end--;

  // Write new null terminator
  *(end + 1) = '\0';

  return str;
}


RnWord* splitwords(const char* input, uint32_t* word_count) {
  // Pass 1: Count words
  size_t input_len = strlen(input);
  *word_count = 0;
  bool in_word = false;

  for (size_t i = 0; i <= input_len; i++) {
    if (isspace(input[i]) || input[i] == '\0') {
      if (in_word) {
        (*word_count)++;
        in_word = false;
      }
    } else {
      in_word = true;
    }
  }

  // Allocate memory for the words
  size_t capacity = *word_count > 2 ? *word_count : 2;
  RnWord* words = (RnWord*)malloc(capacity * sizeof(RnWord));
  memset(words, 0, sizeof(RnWord) * capacity);

  if (!words) {
    fprintf(stderr, "runara: memory allocation failed\n");
    exit(EXIT_FAILURE);
  }

  // Pass 2: Store the words
  size_t start = 0;
  size_t word_idx = 0;
  in_word = false;
  for (size_t i = 0; i <= input_len; i++) {
    if (isspace(input[i]) || input[i] == '\0') {
      if (in_word) {
        size_t word_length = i - start;
        if (word_idx >= capacity) {
          capacity *= 2;
          RnWord* temp = realloc(words, capacity * sizeof(RnWord));
          if (!temp) {
            for (uint32_t j = 0; j < word_idx; j++) {
              free(words[j].str);
            }
            free(words);
            fprintf(stderr, "runara: memory allocation failed\n");
            exit(EXIT_FAILURE);
          }
          words = temp;
        }

        words[word_idx].str = (char*)malloc(word_length + 1);
        if (!words[word_idx].str) {
          for (uint32_t j = 0; j < word_idx; j++) {
            free(words[j].str);
          }
          free(words);
          fprintf(stderr, "runara: memory allocation failed\n");
          exit(EXIT_FAILURE);
        }
        strncpy(words[word_idx].str, input + start, word_length);
        words[word_idx].str[word_length] = '\0';
        words[word_idx].has_newline = (input[i] == '\n');
        word_idx++;
        in_word = false;
      }
    } else {
      if (!in_word) {
        start = i;
      }
      in_word = true;
    }
  }
  return words;
}

RnTextProps rn_text_render_paragraph(
  RnState* state, 
  const char* paragraph,
  RnFont* font, 
  vec2s pos, 
  RnColor color,
  RnParagraphProps props) {
  return rn_text_render_paragraph_ex(
    state, 
    paragraph, 
    font,
    pos, 
    color, 
    props, 
    true); // Render set to true
}

RnTextProps 
rn_text_render_paragraph_ex(
  RnState* state, 
  const char* const_paragraph,
  RnFont* font, 
  vec2s pos, 
  RnColor color,
  RnParagraphProps props,
  bool render) {


  char* paragraph_copy = strdup(const_paragraph);  
  char* paragraph = trimspaces(paragraph_copy);
  RnHarfbuzzText* hb_text = rn_hb_text_from_str(state, *font, paragraph);

  if (!hb_text->highest_bearing) {
    for (unsigned int i = 0; i < hb_text->glyph_count; i++) {
      RnGlyph glyph = rn_glyph_from_codepoint(state, font, hb_text->glyph_info[i].codepoint);
      hb_text->highest_bearing = fmaxf(hb_text->highest_bearing, glyph.bearing_y);
    }
  }

  vec2s start_pos = (vec2s){.x = pos.x, .y = pos.y};
  const int32_t line_feed = 0x000A, carriage_return = 0x000D, line_seperator = 0x2028, paragraph_seperator = 0x2029;

  uint32_t nwords = hb_text->nwords;

  if (!hb_text->words || !nwords) {
    hb_text->words = splitwords((char*)paragraph, &nwords);
  }
  hb_text->nwords = nwords;
  if(!nwords) return (RnTextProps){0};

  float word_ys[nwords];
  memset(word_ys, 0, sizeof(word_ys));
  uint32_t nwraps = 0;
  float x = pos.x, y = pos.y;
  uint32_t _it = 0;
  vec2s paragraph_pos = pos;
  uint32_t word_idx = 0;
  float ylast = -1;
  float textw = 0.0f, linew = 0; 
  int32_t maxasc = 0, maxdec = 0;

  float lw[nwords];
  memset(lw, font->space_w, sizeof(lw));
  bool newline = false;
  for (uint32_t i = 0; i < nwords; i++) {
    bool left = props.align == RN_PARAGRAPH_ALIGNMENT_LEFT;
    if(!hb_text->words[i].width)
      hb_text->words[i].width = rn_text_props(state, hb_text->words[i].str, font).width;

    float word_width = hb_text->words[i].width + font->space_w;
    if (i == nwords - 1) word_width -= font->space_w;

    x += word_width;

    if (((x > props.wrap && props.wrap != -1.0f && 
      nwords > 1 && i != 0) || 
      newline)) {
      y += font->line_h;
      x = pos.x + word_width;
      nwraps++;
      if(!left)
        lw[_it] -= font->space_w;
      _it++;
    }
    if(!left)
      lw[_it] += word_width; 

    newline = hb_text->words[i].has_newline;
    word_ys[i] = y;
  }



  float align_diver = (props.align == RN_PARAGRAPH_ALIGNMENT_CENTER) ? 2.0f : 1.0f;
  if(props.align != RN_PARAGRAPH_ALIGNMENT_LEFT) {
    float centered = start_pos.x + (((props.wrap - start_pos.x)  - lw[0]) / align_diver);
    pos.x = centered; 
    paragraph_pos.x = pos.x;
  }

  if(props.align == RN_PARAGRAPH_ALIGNMENT_CENTER)
    pos.x += font->space_w;



  _it = 1;
  for (uint32_t i = 0; i < hb_text->glyph_count; i++) {
    bool wrapped = false;
    if (!hb_text->glyph_info[i].codepoint) 
      continue;
    RnGlyph glyph = rn_glyph_from_codepoint(state, font, hb_text->glyph_info[i].codepoint);
    hb_glyph_position_t hbpos = hb_text->glyph_pos[i];
    float xadv = hbpos.x_advance / 64.0f;
    float yadv = hbpos.y_advance / 64.0f;
    float xoff = hbpos.x_offset / 64.0f;
    float yoff = hbpos.y_offset / 64.0f;

    uint32_t codepoint_idx = hb_text->glyph_info[i].cluster;
    char codepoint = paragraph[codepoint_idx];

    if (codepoint_idx != strlen(paragraph) - 1 && 
      ((codepoint == ' ' && paragraph[codepoint_idx + 1] != ' ') || 
      (codepoint == '\t' && paragraph[codepoint_idx + 1] != '\t') || 
      (codepoint == '\n' && paragraph[codepoint_idx + 1] != '\n')) &&
      word_idx + 1 < nwords) {
      word_idx++;
      wrapped = false;
    }

    if (ylast != pos.y && ylast != -1.0f) {
      float x = start_pos.x + ((props.align != RN_PARAGRAPH_ALIGNMENT_LEFT) ? 
        ((props.wrap - start_pos.x) - lw[_it++]) / align_diver : 0.0f) + 
        (props.align == RN_PARAGRAPH_ALIGNMENT_CENTER ? font->space_w : 0.0f);  

      pos.x = x;
      if(x < paragraph_pos.x)
        paragraph_pos.x = x;

      linew = 0.0f;
      maxdec = 0;
      maxasc = 0;
    }

    ylast = pos.y;
    if (!wrapped) pos.y = word_ys[word_idx];


    if (codepoint == '\t') {
      pos.x += font->tab_w * font->space_w;
      continue;
    }

    vec2s glyph_pos = {
      pos.x + xoff,
      pos.y + hb_text->highest_bearing - yoff 
    };

    if (render) {
      rn_glyph_render(state, glyph, *font, glyph_pos, color);
    }

    pos.x += xadv;
    pos.y += yadv;
    linew += xadv;

    textw = fmaxf(textw, linew);
    maxasc = fmaxf(maxasc, glyph.ascender);
    maxdec = fminf(maxdec, glyph.descender);
  }

  float last_line_h = maxasc + abs(maxdec);
  return (RnTextProps){
    .width = textw, 
    .height = (nwraps > 0) ? (nwraps * font->line_h + last_line_h) : last_line_h, 
    .paragraph_pos = paragraph_pos
  };

  free(paragraph_copy);
}


void rn_glyph_render(
  RnState* state,
  RnGlyph glyph,
  RnFont font,
  vec2s pos,
  RnColor color) {

  vec2s texcoords[4] = { 
    (vec2s){glyph.u0, glyph.v0}, // Bottom-left
    (vec2s){glyph.u1, glyph.v0}, // Bottom-right
    (vec2s){glyph.u1, glyph.v1}, // Top-right
    (vec2s){glyph.u0, glyph.v1}  // Top-left
  };

  float xpos = pos.x + glyph.bearing_x;
  float ypos = pos.y - glyph.bearing_y;

  RnTexture tex = (RnTexture){
    .id = font.atlas_id,
    .width = glyph.width,
    .height = glyph.height 
  };

  rn_image_render_adv(state, (vec2s){xpos, ypos}, 0.0f,
                      color, tex, texcoords, true,
                      RN_NO_COLOR, 0.0f,
                      0.0f);
}

RnGlyph rn_glyph_from_codepoint(
  RnState* state, 
  RnFont* font, 
  uint64_t codepoint
) {
  return get_glyph_from_cache(&state->glyph_cache, font, codepoint);
}
RnHarfbuzzText* rn_hb_text_from_str(
  RnState* state,
  RnFont font,
  const char* str
) {
  return get_hb_text_from_cache(&state->hb_cache, font, str);
}


void rn_set_cull_end_x(RnState* state, float x) {
  state->cull_end.x = x; 
}

void rn_set_cull_end_y(RnState* state, float y) {
  state->cull_end.y = y; 
}
void rn_set_cull_start_x(RnState* state, float x) {
  state->cull_start.x = x;
}

void rn_set_cull_start_y(RnState* state, float y) {
  state->cull_start.y = y;
}

void rn_unset_cull_start_x(RnState* state) {
  state->cull_start.x = -1;
}

void rn_unset_cull_start_y(RnState* state) {
  state->cull_start.y = -1;
}

void rn_unset_cull_end_x(RnState* state) {
  state->cull_end.x = -1;
}

void rn_unset_cull_end_y(RnState* state) {
  state->cull_end.y = -1;
}

RnTextProps rn_text_render(
  RnState* state, 
  const char* text,
  RnFont* font, 
  vec2s pos, 
  RnColor color) {
  return rn_text_render_ex(state, text, font, pos, color, 0.0f, true);
}

RnTextProps rn_text_render_base_types(
  RnState* state, 
  const char* text,
  RnFont* font, 
  float pos_x,
  float pos_y,
  unsigned char color_r, 
  unsigned char color_g, 
  unsigned char color_b, 
  unsigned char color_a
) {
  return rn_text_render_ex(
    state, text, font, 
    (vec2s){pos_x, pos_y},
    (RnColor){color_r, color_g, color_b, color_a},
    0.0f, true);
}

RnTextProps rn_text_props(
  RnState* state, 
  const char* text, 
  RnFont* font
) {
  return rn_text_render_ex(state, text, font, (vec2s){0, 0},
                           RN_NO_COLOR, 0.0f, false);
}

RnTextProps 
rn_text_props_paragraph(
  RnState* state, 
  const char* text, 
  vec2s pos,
  RnFont* font,
  RnParagraphProps props
) {
  return rn_text_render_paragraph_ex(state, text, font, pos, 
                                     RN_NO_COLOR, props, false);
}

float rn_text_width(
  RnState* state, 
  const char* text, 
  RnFont* font
) {
  return rn_text_props(state, text, font).width;
}

float rn_text_height(
  RnState* state, 
  const char* text, 
  RnFont* font
) {
  return rn_text_props(state, text, font).height;
}

RnColor rn_color_from_hex(uint32_t hex) {
  RnColor color;
  color.r = (hex>> 16) & 0xFF;
  color.g = (hex >> 8) & 0xFF; 
  color.b = hex& 0xFF; 
  color.a = 255; 
  return color;
}

uint32_t rn_color_to_hex(RnColor color) {   
  return ((uint32_t)color.r << 24) | 
  ((uint32_t)color.g << 16) | 
  ((uint32_t)color.b << 8)  | 
  ((uint32_t)color.a << 0);
}

RnColor rn_color_from_zto(vec4s zto) {
  return (RnColor){
    (uint8_t)(zto.r * 255.0f), 
    (uint8_t)(zto.g * 255.0f), 
    (uint8_t)(zto.b * 255.0f), 
    (uint8_t)(zto.a * 255.0f)};
}

vec4s rn_color_to_zto(RnColor color) {
  return (vec4s){color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
}

