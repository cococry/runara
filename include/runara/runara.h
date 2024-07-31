#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <cglm/cglm.h>
#include <cglm/struct.h>
#include <libclipboard.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#define RN_NO_COLOR (LfColor){0, 0, 0, 0}
#define RN_WHITE (LfColor){255, 255, 255, 255}
#define RN_BLACK (LfColor){0, 0, 0, 255}
#define RN_RED (LfColor){255, 0, 0, 255}
#define RN_GREEN (LfColor){0, 255, 0, 255}
#define RN_BLUE (LfColor){0, 0, 255, 255}

#define RN_MAX_RENDER_BATCH 10000
#define RN_MAX_TEX_COUNT_BATCH 32

typedef void* (*LfGLLoader)(const char *name);

// -- Struct Defines ---
typedef struct {
  uint32_t id;
} LfShader;

typedef struct {
    uint32_t id;
    uint32_t width, height;
} LfTexture;

typedef struct {
  FT_Face face;
  hb_font_t* hb_font;
  uint32_t size, line_height;
} LfFont;

typedef struct {
  uint32_t id;
  uint32_t width, height;
  int32_t bearing_x, bearing_y;
  int32_t advance;
  uint64_t codepoint;
} LfGlyph;

typedef struct {
  hb_buffer_t* buf;
  hb_glyph_info_t* glyph_info; 
  hb_glyph_position_t* glyph_pos; 
  uint32_t glyph_count;
  uint64_t hash;
} LfHarfbuzzText;

typedef struct {
  LfGlyph* glyphs;
  size_t size, cap;
} LfGlyphCache;

typedef struct {
  LfHarfbuzzText* texts;
  size_t size, cap;
} LfHarfbuzzCache;

typedef struct {
  vec2 pos; // 8 Bytes
  vec4 border_color; // 16 Bytes
  float border_width; // 4 Bytes 
  vec4 color; // 16 Bytes
  vec2 texcoord; // 8 Bytes
  float tex_index; // 4 Bytes
  vec2 scale; // 8 Bytes
  vec2 pos_px; // 8 Bytes
  float corner_radius; // 4 Bytes
  vec2 min_coord, max_coord; // 16 Bytes
} LfVertex; // 88 Bytes per vertex

// State of the batch renderer
typedef struct {
  LfShader shader;
  uint32_t vao, vbo, ibo;
  uint32_t vert_count;
  LfVertex* verts;
  vec4s vert_pos[4];
  LfTexture textures[RN_MAX_TEX_COUNT_BATCH];
  uint32_t tex_index, tex_count,index_count;
} LfRenderState;


typedef struct {
    uint8_t r, g, b, a;
} LfColor;

typedef enum {
    RN_TEX_FILTER_LINEAR = 0,
    RN_TEX_FILTER_NEAREST
} LfTextureFiltering;

typedef struct {
  float width, height;
} LfTextProps; 

typedef struct {
  bool init;

  // Rendering
  LfRenderState render;
  vec2s cull_start, cull_end;
  uint32_t drawcalls;

  uint32_t render_w, render_h;

  // Text rendering
  FT_Library ft;
  LfGlyphCache glyph_cache;
  LfHarfbuzzCache hb_cache;
} LfState;

LfState rn_init(uint32_t render_w, uint32_t render_h, LfGLLoader loader);

void rn_terminate(LfState* state);

void rn_resize_display(LfState* state, uint32_t render_w, uint32_t render_h);

LfTexture rn_load_texture(const char* filepath, bool flip, LfTextureFiltering filter);

LfFont rn_load_font(LfState* state, const char* filepath, uint32_t size, uint32_t line_height);

void rn_free_texture(LfTexture* tex);

void rn_free_font(LfFont* font);

void rn_begin(LfState* state);

void rn_end(LfState* state);

void rn_rect_render(LfState* state, 
    vec2s pos, 
    vec2s size, 
    LfColor color, 
    LfColor border_color, 
    float border_width, 
    float corner_radius);

void rn_image_render(
    LfState* state, 
    vec2s pos, 
    LfColor color, 
    LfTexture tex, 
    LfColor border_color, 
    float border_width, 
    float corner_radius);

LfTextProps rn_text_render_ex(
    LfState* state, 
    const char* text,
    LfFont font, 
    vec2s pos, 
    LfColor color,
    bool render);

void rn_glyph_render(
    LfState* state,
    LfGlyph glyph,
    LfFont font, 
    vec2s pos,
    LfColor color);

LfGlyph rn_glyph_from_codepoint(
    LfState* state, 
    LfFont font, 
    uint64_t codepoint
    );

LfTextProps rn_text_render(
    LfState* state, 
    const char* text,
    LfFont font, 
    vec2s pos, 
    LfColor color);

LfTextProps rn_text_props(
    LfState* state, 
    const char* text, 
    LfFont font
    );

int32_t rn_utf8_to_codepoint(const char *utf8, uint64_t *codepoint);

void rn_set_cull_start_x(LfState* state, float x);

void rn_set_cull_start_y(LfState* state, float y);

void rn_set_cull_end_x(LfState* state, float x);

void rn_set_cull_end_y(LfState* state, float y);  

void rn_unset_cull_start_x(LfState* state);

void rn_unset_cull_start_y(LfState* state);

void rn_unset_cull_end_x(LfState* state);

void rn_unset_cull_end_y(LfState* state);

LfColor rn_color_brightness(LfColor color, float brightness);

LfColor rn_color_alpha(LfColor color, uint8_t a);

vec4s rn_color_to_zto(LfColor color);

LfColor rn_color_from_hex(uint32_t hex);

LfColor rn_color_from_zto(vec4s zto);
