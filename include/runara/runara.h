#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <cglm/cglm.h>
#include <cglm/struct.h>
#include <libclipboard.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_BITMAP_H
#include FT_COLOR_H

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>


// -- Macros ---

// Defines a color that is zero'd out (no alpha, so not visible)
#define RN_NO_COLOR (RnColor){0, 0, 0, 0}
// Defines a color that is set to 255 across all channels
#define RN_WHITE (RnColor){255, 255, 255, 255}
// Defines a color that is set to 0 to all channels except the 
// alpha channel is set to 255
#define RN_BLACK (RnColor){0, 0, 0, 255}
// Defines a color that has the alpha and red channel set to 255.
// Other channels are set to 0 
#define RN_RED (RnColor){255, 0, 0, 255}
// Defines a color that has the alpha and green channel set to 255. 
// Other channels are set to 0
#define RN_GREEN (RnColor){0, 255, 0, 255}
// Defines a color that has the alpha and blue channel set to 255. 
// Other channels are set to 0
#define RN_BLUE (RnColor){0, 0, 255, 255}

// Defines the count of rendered elements in the batch renderer 
// before it creates a new batch.
#define RN_MAX_RENDER_BATCH 10000
// Defines the maximum number of textures with different IDs that can 
// be rendered within one batch in the batch renderer.
#define RN_MAX_TEX_COUNT_BATCH 32

// This function type is used as a drop-in replacement for the 
// GLADloadproc type.
typedef void* (*RnGLLoader)(const char *name);

// --- Enumartions --- 
/**
 * @enum RnTextureFiltering 
 * @brief Enumartion of different modes
 * that are used to filter OpenGL textures
 */
typedef enum {
  // Smoothes textures by averaging colors of 
  // surrounding texels, reducing pixelation and 
  // producing a more blended appearance.
  RN_TEX_FILTER_LINEAR = 0,
  // Selects the color of the nearest texel without 
  // averaging, resulting in a blocky, pixelated 
  // texture appearance.
  RN_TEX_FILTER_NEAREST
} RnTextureFiltering;

/**
 * @enum RnParagraphAlignment 
 * @brief Enumartion of different alignments 
 * of rendered paragraphs.
 * - RN_PARAGRAPH_ALIGNMENT_LEFT alignes text inside 
 * the paragraph on the left (starting X coordinate)
 *
 * - RN_PARAGRAPH_ALIGNMENT_RIGHT alignes text inside 
 * the paragraph on the right (at wrap point)
 *
 * - RN_PARAGRAPH_ALIGNMENT_CENTER centeres lines inside the 
 * paragraph (alignment around center point)
 *
 */
typedef enum {
  RN_PARAGRAPH_ALIGNMENT_LEFT = 0,
  RN_PARAGRAPH_ALIGNMENT_CENTER,
  RN_PARAGRAPH_ALIGNMENT_RIGHT,
} RnParagraphAlignment;


// --- Structs ---

/**
 * @struct RnShader 
 * @brief Represents a OpenGL shader object  
 *
 * This structure is a wrapper around the data that 
 * an OpenGL shader uses. 
 */
typedef struct {
  // The OpenGL object ID of the shader
  uint32_t id;
} RnShader;

/**
 * @struct RnWord
 * @brief Represents a word within a rendered paragraph 
 *
 */

typedef struct {
  // The string contents of the word
  char* str;       
  // Whether or not a new-line follows after 
  // the word 
  bool has_newline; 
  // The width in pixels of the rendered word 
  // with the font associated to it via the harfbuzz 
  // text 
  float width;
} RnWord;

/**
 * @struct RnTexture 
 * @brief Represents a renderable texture object  
 *
 * This structure encompasses the data that is required to 
 * render a texture within the batch renderer.
 * an OpenGL shader uses.
 */
typedef struct {
  // The OpenGL object ID of the texture 
  uint32_t id;
  // The width of the texture (in pixels)
  uint32_t width;
  // The height of the texture (in pixels)
  uint32_t height;
} RnTexture;

/**
 * @struct RnFont
 * @brief Represents the data of a font used for rendering 
 *
 * This structure encompasses metadata aswell as FreeType and 
 * HarfBuzz handles that are needed during text rendering with 
 * the font.
 */
typedef struct {
  // The FreeType font face data
  FT_Face face;
  // The HarfBuzz font data
  hb_font_t* hb_font;
  // The pixel size of the font
  uint32_t size;
  // The size of the selected strike for Emoji/Colored fonts 
  uint32_t selected_strike_size;
  // The ID of the font
  uint32_t id;
  // The with in pixels of a space character 
  // within the font
  float space_w;
  // The height in pixels that is advanced after a line 
  // of rendered text in a paragraph
  float line_h;
  // The number of spaces that are used 
  // to represent a tab character (default is 4)
  uint32_t tab_w;
  // The width (in pixels) of the glyph texture atlas 
  // (default is 1024)
  uint32_t atlas_w;
  // The height (in pixels) of the glyph texture atlas 
  // (default is 1024)
  uint32_t atlas_h;
  // The height of rows within the 
  // glyph texture atlas 
  uint32_t atlas_row_h;
  // The X position to insert newly loaded 
  // glyphs into the atlas to
  uint32_t atlas_x;
  // The Y position to insert newly loaded 
  // glyphs into the atlas to
  uint32_t atlas_y;
  // The OpenGL object ID of the 
  // atlas texture
  uint32_t atlas_id;
  // The OpenGL filtering mode of the texture 
  // atlas of the font
  RnTextureFiltering filter_mode;
  // The path of the font's file 
  char* filepath;
  // The face index of the loaded font face 
  uint32_t face_idx;

} RnFont;

/**
 * @struct RnGlyph
 * @brief Represents the data that is needed to render a glyph 
 *
 * This structure is used to cache glyph information of a given
 * codepoint in a font.
 *
 */
typedef struct {
  // The width of the glyphs texture
  uint32_t width;
  // The height of the glyphs texture
  uint32_t height;
  float glyph_top;
  float glyph_bottom;
  // The horizontal offset of the glyph's origin from the
  // font's x-axis.
  int32_t bearing_x; 
  // The vertical offset of the glyph's origin from the 
  // font's baseline.
  int32_t bearing_y;
  // The horizontal distance to move the cursor after 
  // rendering the glyph.
  int32_t advance;
  // The codepoint value of the glyph within the font .
  // Used as glyph index within the font
  uint64_t codepoint;
  // The ID of the font which the glyph was loaded with
  uint64_t font_id; 
  // The X coordinate of the bottom left corner of 
  // the glyphs texture on the texture atlas of it's 
  // font.
  float u0;
  // The Y coordinate of the bottom left corner of 
  // the glyphs texture on the texture atlas of it's 
  // font.
  float v0;
  // The X coordinate of the top right corner of 
  // the glyphs texture on the texture atlas of it's 
  // font.
  float u1; 
  // The Y coordinate of the top right corner of 
  // the glyphs texture on the texture atlas of it's 
  // font.
  float v1;

  int32_t ascender;

  int32_t descender;
} RnGlyph;

/**
 * @struct RnHarfbuzzText
 * @brief Represents the data that HarfBuzz calculates to 
 * shape and position a given text.
 *
 * This structure is used to cache data that is used to shape and 
 * position a given text. The structure contains a hash by which it 
 * is identified within the cache. The hash is generated by the text 
 * that is rendered.
 */
typedef struct {
  // The HarfBuzz buffer of the txt
  hb_buffer_t* buf;
  // The HarfBuzz glyph information (codepoints) of the text
  hb_glyph_info_t* glyph_info; 
  // The positions of the individual glyphs
  hb_glyph_position_t* glyph_pos; 
  // The number of rendered glyphs
  uint32_t glyph_count;
  // The hash generated by the rendered text
  uint64_t hash;
  // The ID of the font with which the text 
  // information were generated 
  uint32_t font_id;
  // The text that is rendered with the harfbuzz 
  // information
  char* str;
  // The highest glyph bearing within the text
  float highest_bearing;

  // A list of all words split by space characters that 
  // are within the paragraph 
  RnWord* words;
  uint32_t nwords;
} RnHarfbuzzText;

/**
 * @struct RnVertex 
 * @brief Defines the data that a vertex uses within the 
 * batch renderer.
 *
 * This structure is used to store information about a 
 * rendered vertex and communicate it to the GPU.
 */
typedef struct {
  // The position of the vertex in pixel space 
  vec2 pos;             // 8 Bytes
  // The border color of the vertex (ZTO)
  vec4 border_color;    // 16 Byes
  // The border width of the vertex (px)
  float border_width;   // 4 Bytes
  // The color of the vertex (ZTO)
  vec4 color;           // 16 Bytes
  // The texture coordinates of the 
  // vertex (NDC)
  vec2 texcoord;        // 8 Bytes
  // The index of the vertex's texture
  // within the current batch. This is 
  // set to -1.0 when no texture is rendered 
  // with the vertex.
  float tex_index;      // 4 Bytes
  // The size of the rectangle that this 
  // vertex is associated to (pixel space)
  vec2 size_px;         // 8 Bytes
  // The positon of the rectangle that this 
  // vertex is associated to (pixel space)
  vec2 pos_px;          // 8 Bytes
  // The corner radius of the vertex (px)
  float corner_radius;  // 4 Bytes
  // Specifies if the vertex is rendering a text (
  // 1.0 if text, 0.0 if not)
  float is_text;

  // Specifies the starting position from where to cull the 
  // shape that contains the vertex 
  vec2 min_coord;
  // Specifies the ending position from where to cull the 
  // shape that contains the vertex 
  vec2 max_coord;
} RnVertex; // 92 Bytes per vertex

/**
 * @struct RnRenderState 
 * @brief Defines the state of the 2D batch renderer 
 * that Runara uses to display rectangles and textures.
 *
 * This structure contains handles the to OpenGL objects
 * that are used to communate the CPU-side vertex data 
 * within a batch to the GPU. 
 *
 * The structure also stores the vertex data aswell as 
 * the textures that are rendered within the current 
 * batch.
 */
typedef struct {
  // The OpenGL shader that is used for 
  // rendering batches
  RnShader shader;
  // The OpenGL object ID of the vertex array 
  // that is used to render a batch.
  uint32_t vao;
  // The OpenGL object ID of the vertex buffer 
  // that is used to communicate vertices 
  // to the GPU.
  uint32_t vbo;
  // The OpenGL object ID of the index buffer 
  // that is used to index vertices.
  uint32_t ibo;
  // The number of vertices in the current batch
  uint32_t vert_count;
  // The vertex data in the current batch
  RnVertex* verts;
  // The vertex positions that make up a quad (NDC)
  vec4s vert_pos[4];
  // The textures that are rendered within the 
  // current batch
  RnTexture textures[RN_MAX_TEX_COUNT_BATCH];
  // The index to insert at which to insert 
  // new textures
  uint32_t tex_index;
  // The number of textures in the current batch
  uint32_t tex_count;
  // The number of indices within the 
  // current batch
  uint32_t index_count;
} RnRenderState;

/**
 * @struct RnColor 
 * @brief Defines a simple RGBA color helper
 * that has a 0-255 range per color channel. 
 */
typedef struct {
  // The red channel of the color
  uint8_t r;
  // The green channel of the color
  uint8_t g;
  // The blue channel of the color
  uint8_t b;
  // The alpha channel of the color
  uint8_t a;
} RnColor;



/**
 * @struct RnTextProps
 * @brief Specifies the width and height of a 
 * rendered text.
 */
typedef struct {
  // The width of the text
  float width;
  // The height of the text
  float height;

  // The position of the rendered 
  // pargraph with alignment applied (e.g centered text)
  vec2s paragraph_pos;
} RnTextProps; 

/**
 * @struct RnGlyphCache 
 * @brief Simple dynamic 
 * array structure to cache glyphs
 */
typedef struct {
  RnGlyph* glyphs;
  size_t size, cap;
} RnGlyphCache;

/**
 * @struct RnGlyphCache 
 * @brief Simple dynamic 
 * array structure to cache 
 * harfbuzz text information
 */
typedef struct {
  RnHarfbuzzText** texts;
  size_t size, cap;
} RnHarfbuzzCache;


/**
 * @struct RnState 
 * @brief Specifies the data that is used for all 
 * functions that use the Runara API. This includes
 * mostly rendering state and caches.
 *
 * This structure stores the state of the batch renderer,
 * the dimensions of the rendered area and text rendering 
 * caches. 
 */
typedef struct {
  // States if the library has already been 
  // initialized (Set to true after 'rn_init()')
  bool init;

  // The state of the batch renderer
  RnRenderState render;
  // The amount of drawcalls in the current 
  // batch. The value of this variable will 
  // only be correct if used after 'rn_end()'
  uint32_t drawcalls;

  // The width of the rendered area
  uint32_t render_w;
  // The height of the rendered area
  uint32_t render_h;

  // The FreeType handle used for loading 
  // fonts
  FT_Library ft;
  // The data of cached glyphs
  RnGlyphCache glyph_cache;
  // The data of cached texts shaped 
  // with HarfBuzz
  RnHarfbuzzCache hb_cache;
  // The ID that is used for the next 
  // loaded font (incremented if a font was loaded)
  uint32_t font_id;

  // The starting position of the active culling box (-1,-1 when no culling box) 
  vec2s cull_start;
  // The ending position of the active culling box (-1,-1 when no culling box) 
  vec2s cull_end;
} RnState;

/**
 * @struct RnParagraphProps
 * @brief Specifies the properties of a paragraph rendered 
 * with rn_text_render_paragraph(_ex)
 *
 * This structure contains information about how to layout a 
 * rendered paragraph.
 }
 */
typedef struct {
  // The alignment of the individual lines inside the paragraph 
  // (left alignment, right alignment, centered text)
  RnParagraphAlignment align;
  // The X coordinate where lines break 
  // relative to the starting X coordinate of the paragraph
  float wrap;
} RnParagraphProps;

// --- Functions ---

/**
 * @brief Initializes the Runara library 
 *
 * This function sets up initial variable values, 
 * the OpenGL batch renderer and the FreeType library 
 * used for loading fonts.
 *
 * @param[in] render_w The width of the area on which Runara will render 
 * @param[in] render_h The height of the area on which Runara will render
 * @param[in] loader The function to load OpenGL with 
 * 
 * @return The initialized state of the library.
 * The .init member of the struct is set to false 
 * if the library failed to initalize
 * 
 * @example
 * RnState state = rn_init(800, 600, glfwGetProcAddress);
 */
RnState* rn_init(uint32_t render_w, uint32_t render_h, RnGLLoader loader);

/**
 * @brief Terminates the Runara library 
 *
 * This functions deallocates memory and 
 * state allocated by Runara.
 *
 * @param[in] state The state of the library 
 */
void rn_terminate(RnState* state);

/**
 * @brief Resizes the area on which Runara 
 * renders.
 *
 * This functions recalculates the projection 
 * matrix for the rendered contents and sets
 * the OpenGL viewport with the given area.
 *
 *
 * @param[in] state The state of the library 
 * @param[in] render_w The width of new render area 
 * @param[in] render_h The height of new render area 
 * 
 */
void rn_resize_display(RnState* state, uint32_t render_w, uint32_t render_h);

/**
 * @brief Loads a image on a given filepath into a OpenGL 
 * texture object and returns a Runara Texture handle.
 *
 * This function uses stb_image to load the image at 
 * a given filepath into an OpenGL texture. The returned
 * texture handle represents the loaded Texture:
 *  .id member is set to the OpenGL object ID of the texture
 *  .width member is set to the width (in px) of the loaded texture 
 *  .height member is set to the height (in px) of the loaded texture
 *
 *
 * @param[in] filepath The filepath of the image 
 * to load the OpenGL texture off of
 * @param[in] flip Whether or not to flip the loaded texture data 
 * (Set to true to make the texture appear not flipped)
 * @param[in] filter The filtering mode of the loaded texture
 *
 * @return The loaded texture
 * 
 */
RnTexture rn_load_texture_ex(const char* filepath, bool flip, RnTextureFiltering filter);

/**
 * @brief Uses 'rn_load_texture_ex' with 'flip'
 * set to true and 'filter' set to 'RN_TEX_FILTER_LINEAR'
 *
 * @param[in] filepath The filepath of the image 
 * to load the OpenGL texture off of
 *
 * @return The loaded texture
*/
RnTexture rn_load_texture(const char* filepath);

/**
 * @brief Loads a texture from a given filepath and assigns the 
 * texture ID, width and height to the given argument pointers. 
 *
 * @param[in] filepath The filepath of the image 
 * to load the OpenGL texture off of
 *
 * @param[out] o_tex_id The OpenGL texture ID of the 
 * loaded texture.
 * @param[out] o_tex_width The width of the 
 * loaded texture.
 * @param[out] o_tex_height The height of the 
 * loaded texture.
 *
 * @param[in] filter The filter/ng mode used for the texture 
 * ( 0 => RN_TEX_FILTER_LINEAR
 *   1 => RN_TEX_FILTER_NEAREST )
 *
*/
void rn_load_texture_base_types(
    const char* filepath, 
    uint32_t* o_tex_id, 
    uint32_t* o_tex_width, 
    uint32_t* o_tex_height,
    uint32_t filter);


/**
 * @brief Loads a font from a given filepath with FreeType 
 * and creates the initial empty texture atlas with the 
 * given dimensions.
 *
 * This functions loads a font face on a given filepath 
 * with the FreeType library and creates the OpenGL texture 
 * for the atlas of the texture with the given dimensions.
 *
 * @param[in] state The state of the library
 * @param[in] filepath The filepath of which to load the font 
 * off of.
 * @param[in] size The pixel size to load the font face with
 * @param[in] atlas_w The width (in px) of the texture atlas
 * @param[in] atlas_h The height (in px) of the texture atlas
 * @param[in] tab_w The amount of spaces to use for a tab character
 * @param[in] filter_mode The OpenGL filtering mode of the texture  
 * @param[in] face_idx The index of the font to load (0 is the default face) 
 * atlas of the font
 *
 * @return The loaded font
 * 
 */
RnFont* rn_load_font_ex(RnState* state, const char* filepath, uint32_t size,
    uint32_t atlas_w, uint32_t atlas_h, uint32_t tab_w, RnTextureFiltering filter_mode, 
    uint32_t face_idx);

/**
 * @brief Creates a font from an already loaded FreeType font face 
 * and Harfbuzz font handle, and generates the corresponding texture atlas.
 *
 * This function allows creating a font object by providing an already 
 * loaded FreeType font face and Harfbuzz font handle. It then creates 
 * the OpenGL texture atlas for the font and sets the necessary 
 * attributes for rendering, including space width and line height.
 *
 * INFO: The `space_w` parameter allows you to specify the width of the 
 * space character, which is commonly precomputed.
 *
 * @param[in] state The state of the library.
 * @param[in] face The pre-loaded FreeType font face.
 * @param[in] hb_font The pre-loaded Harfbuzz font handle.
 * @param[in] size The pixel size to use for rendering the font.
 * @param[in] atlas_w The width of the font's OpenGL texture atlas.
 * @param[in] atlas_h The height of the font's OpenGL texture atlas.
 * @param[in] tab_w The width of the tab character.
 * @param[in] filter_mode The texture filtering mode for the atlas.
 * @param[in] face_idx The index of the font face (useful if the font file 
 * contains multiple faces, such as bold or italic).
 * @param[in] filepath The filepath of the font, used for debugging purposes.
 * @param[in] space_w The width of the space character.
 *
 * @return A pointer to the created `RnFont` object.
 */
RnFont* rn_create_font_from_loaded_data_ex(RnState* state, FT_Face face, hb_font_t* hb_font,
                                        uint32_t size, uint32_t atlas_w, uint32_t atlas_h, 
                                        uint32_t tab_w, RnTextureFiltering filter_mode, 
                                        uint32_t face_idx, const char* filepath, float space_w);

/**
 * @brief Uses 'rn_load_font_from_data_ex' with 'atlas_w' & 'atlas_h'
 * set to 1024. 'tab_w' is set to 4
 *
 * @param[in] state The state of the library
 * @param[in] filepath The filepath of which to load the font 
 * off of.
 * @param[in] size The pixel size to load the font face with
 *
 * @return The loaded font
*/

RnFont* rn_create_font_from_loaded_data(RnState* state, FT_Face face, hb_font_t* hb_font, float space_w,
                                        uint32_t size, 
                                        uint32_t face_idx, const char* filepath);

/**
 * @brief Uses 'rn_load_font_ex' with 'atlas_w' & 'atlas_h'
 * set to 1024. 'tab_w' is set to 4
 *
 * @param[in] state The state of the library
 * @param[in] filepath The filepath of which to load the font 
 * off of.
 * @param[in] size The pixel size to load the font face with
 *
 * @return The loaded font
*/
RnFont* rn_load_font(RnState* state, const char* filepath, uint32_t size);

/**
 * @brief Loads a font from a given filepath and font face index with FreeType 
 * and creates the initial empty texture atlas with the 
 * given dimensions.
 *
 * This functions loads a font face on a given filepath 
 * with the FreeType library and creates the OpenGL texture 
 * for the atlas of the texture with the given dimensions.
 *
 * INFO: The font face index usualy specifies the variation of 
 * font face within the font file that will be loaded (e.g bold, italic, oblique)
 *
 * @param[in] state The state of the library
 * @param[in] filepath The filepath of which to load the font 
 * off of.
 * @param[in] size The pixel size to load the font face with
 * @param[in] face_idx The index of the font to load (0 is the default face) 
 *
 * @return The loaded font
 * 
 */
RnFont* rn_load_font_from_face(RnState* state, const char* filepath, uint32_t size, uint32_t face_idx);

/*
 * @brief Sets the pixel size of a given font.
 *
 * This function uses FreeType to set the pixel size 
 * of a font's glyph face and reloads the glyph- and 
 * harfbuzz-cache of the font.
 *
 * @param[in] state The stte of the library
 * @param[in] font The font of which to set the pixel size of 
 * @param[in] size The pixel size to set the font to
 * */
void rn_set_font_size(RnState* state, RnFont* font, uint32_t size); 

/*
 * @brief Deallocates the OpenGL texture object 
 * associated with the ID of a given texture.
 *
 * This function deletes the OpenGL object associated
 * with the ID of a given texture and memet's the 
 * given texture pointer to zero.
 *
 * @param[in] tex The texture to deallocate
 * */
void rn_free_texture(RnTexture* tex);

/*
 * @brief Deallocates all memory that is allocated 
 * for the given font.
 *
 * This functions frees the FreeType face of the font,
 * destroys the harfbuzz font handle and deltes the 
 * atlas texture. It also deallocates all cached 
 * glyphs of the font and the cached harfbuzz text 
 * data.
 *
 * @param[in] state The state of the library
 * @param[in] font The font to deallocate
 * */
void rn_free_font(RnState* state, RnFont* font);

/*
 * @brief Clears the OpenGL color buffer and 
 * fills it with a given color
 *
 * @param[in] color The color to clear the OpenGL 
 * screen with
 * */

void rn_clear_color(RnColor color);

/*
 * @brief Begins an OpenGL scissor mode by 
 * enabeling GL_SCISSOR_TEST and calling glScissor 
 *
 * NOTE: OpenGL uses the lower left corner as Y=0, 
 * this function uses upper left, like all other functions 
 * in runara.
 *
 * @param[in] pos The starting position of the 
 * scissored box
 * @param[in] size The size of the scissored box
 * @param[in] render_height The height of the rendering area 
 * */
void rn_begin_scissor(vec2s pos, vec2s size, uint32_t render_height);

/*
 * @brief Wrapper around glDisable(GL_SCISSOR_TEST),
 * effectively removing the scissored box created by 
 * rn_begin_scissor.
 * */
void rn_end_scissor(void);

/*
 * @brief Clears the OpenGL color buffer and 
 * fills it with r, g, b and a float values
 * Channel range: 0-255 
 *
 * @param[in] r The red channel of the color
 * @param[in] g The green channel of the color
 * @param[in] b The blue channel of the color
 * @param[in] a The alpha channel of the color
 * */
void rn_clear_color_base_types(
    unsigned char r, 
    unsigned char g, 
    unsigned char b, 
    unsigned char a);

/*
 * @brief Begins rendering operations with 
 * Runara
 *
 * This functions sets up initial variables 
 * of the batch renderer to start rendering.
 *
 * @param[in] state The state of the library
 * */
void rn_begin(RnState* state);

/*
 * @brief Ends the current batch within 
 * the batch renderer and starts a new one.
 *
 * @param[in] state The state of the library
 * */
void rn_next_batch(RnState* state);

/*
 * @brief Adds a vertex with specified attributes
 * to the current batch within the renderer.
 *
 * @param[in] state The state of the library
 * @param[in] vert_pos The position of the vertex in NDC
 * Use state->render.vert_pos for preset vertex positions.
 * @param[in] transform The transform matrix of the vertex
 * @param[in] pos The pixel space position associated with 
 * the vertex
 * @param[in] size The pixel space size of the rectangle 
 * associated with the vertex
 * @param[in] color The color of the vertex
 * @param[in] border_color The border color of the vertex
 * @param[in] border_width The border width of the vertex  
 * @param[in] corner_radius The corner radius of the vertex
 * @param[in] texcoord The texture coordinates of the vertex
 * @param[in] The index of the vertex's texture within the 
 * current batch. (See rn_tex_index_from_tex())
 *
 * @return The created vertex
 * */
RnVertex* rn_add_vertex_ex(
    RnState* state, 
    vec4s vert_pos,
    mat4 transform,
    vec2s pos, 
    vec2s size, 
    RnColor color,
    RnColor border_color,
    float border_width,
    float corner_radius,
    vec2s texcoord, 
    float tex_index,
    bool is_text);

/*
 * @brief Uses 'rn_add_vertex_ex()' with texcoords 
 * zero'd out and tex_index set to -1.
 *
 * @param[in] state The state of the library
 * @param[in] vert_pos The position of the vertex in NDC
 * Use state->render.vert_pos for preset vertex positions.
 * @param[in] transform The transform matrix of the vertex
 * @param[in] pos The pixel space position associated with 
 * the vertex
 * @param[in] size The pixel space size of the rectangle 
 * associated with the vertex
 * @param[in] color The color of the vertex
 * @param[in] border_color The border color of the vertex
 * @param[in] border_width The border width of the vertex  
 * @param[in] corner_radius The corner radius of the vertex
 *
 * @return The created vertex
 * *
 * */
RnVertex* rn_add_vertex(
    RnState* state, 
    vec4s vert_pos,
    mat4 transform,
    vec2s pos, 
    vec2s size, 
    RnColor color,
    RnColor border_color,
    float border_width,
    float corner_radius);

/*
 * @brief Creates a transform 
 * matrix from a scale and translate 
 * matrix created with the given position and 
 * size.
 *
 * @param[in] pos The position of the shape 
 * @param[in] pos The size of the shape 
 * @param[in] rotation_angle The rotation angle (in degrees) of the shape 
 * @param[out] transform The created transform matrix
 * */
void rn_transform_make(vec2s pos, vec2s size, float rotation_angle, mat4* transform);

/*
 * @brief Returns the index of 
 * a given texture within the current 
 * batch.
 *
 * @param[in] state The state of the library 
 * @param[in] tex The texture to get the index from 
 *
 * @return The index (as float) of the texture within
 * the batch. (-1.0 if the texture is not in the batch)
 * */
float rn_tex_index_from_tex(RnState* state, RnTexture tex);

/*
 * @brief Simple wrapper function 
 * that adds a given texture to the 
 * textures rendered in the current batch.
 *
 * @param[in] state The state of the library 
 * @param[in] tex The texture to add to the render-batch 
 * to.
 * */
void rn_add_tex_to_batch(RnState* state, RnTexture tex);

/*
 * @brief Ends rendering operations with 
 * Runara.
 * 
 * This function flushes the batch renderer,
 * submitting all rendered content after 
 * 'rn_begin()' to be displayed.
 *
 * @param[in] state The state of the library 
 * to.
 * */
void rn_end(RnState* state);

/*
 * @brief Reloads the glyph cache 
 * of a given font.
 * 
 * This function recreates the texture atlas 
 * of a given font and reloads all cached 
 * glyphs.
 *
 * @param[in] state The state of the library 
 * @param[in] font The font to reload glyphs of 
 * to.
 * */
void rn_reload_font_glyph_cache(RnState* state, RnFont* font);

/*
 * @brief Reloads the harfbuzz cache 
 * of a given font.
 *
 * This function recreates all 
 * harfbuzz buffers used for glyph 
 * positioning for the given font.
 *
 * @param[in] state The state of the library 
 * @param[in] font The font to reload harfbuzz 
 * text informations of.
 * to.
 * */
void rn_reload_font_harfbuzz_cache(RnState* state, RnFont font);

/*
 * @brief Renders a rectangle 
 * with a given color, border-color (and -width),
 * and corner radius. 
 *
 * This functions adds 4 vertices that represent 
 * a quad to the current render-batch with the 
 * given data.
 *
 * @param[in] state The state of the library 
 * @param[in] pos The position of the rectangle (px) 
 * @param[in] size The size of the rectangle (px)
 * @param[in] rotation_angle The rotation angle of the rectangle (degrees)
 * @param[in] color The color of the rectangle 
 * @param[in] border_color The border color of 
 * the rectangle (ignored if border_width <= 0)
 * @param[in] corner_radius The radius to round 
 * edges with (0 => no rounded corners)
 * */
void rn_rect_render_ex(
    RnState* state, 
    vec2s pos, 
    vec2s size,
    float rotation_angle,
    RnColor color, 
    RnColor border_color, 
    float border_width,
    float corner_radius);

/*
 * @brief Uses 'rn_rect_render_ex()' with 
 * 'border_color set to 'RN_NO_COLOR' and 
 * 'border_width' & 'corner_radius' set to zero. 
 *
 * @param[in] state The state of the library 
 * @param[in] pos The position of the rectangle (px)
 * @param[in] size The size of the rectangle (px)
 * @param[in] color The color of the rectangle 
 * */
void rn_rect_render(
    RnState* state, 
    vec2s pos, 
    vec2s size, 
    RnColor color);

/*
 * @brief Uses 'rn_rect_render()' but with base types.
 *
 * @param[in] state The state of the library 
 * @param[in] posx The X position of the rectangle (px)
 * @param[in] posy The Y position of the rectangle (px)
 * @param[in] width The width of the rectangle (px)
 * @param[in] height The height of the rectangle (px)
 * @param[in] rotation_angle The rotation angle of the rectangle (degrees)
 * @param[in] color_r The red channel of the rectangle 
 * @param[in] color_g The green channel of the rectangle 
 * @param[in] color_b The blue channel of the rectangle 
 * @param[in] color_a The alpha channel of the rectangle 
 * */
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
    unsigned char color_a);

/*
 * @brief Renders a given texture on 
 * a rectangle.
 *
 * This function renders the texture associated
 * with the .id member of the given texture on 
 * a rectangle with the dimensions of
 * tex.width x tex.height.
 *
 * @param[in] state The state of the library 
 * @param[in] pos The position of the image (px)
 * @param[in] color The color of the image 
 * @param[in] tex The texture to render 
 * @param[in] texcoords The texture coordinates 
 * to use to render the image (NDC).
 * @param[in] border_color The border color 
 * of the image (ignored if border_width <= 0).
 * @param[in] border_width The border width 
 * of the rimage
 * @param[in] corner_radius The radius to round 
 * edges with (0 => no rounded corners)
 * */
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
    float corner_radius);

/*
 * @brief Uses 'rn_image_render_adv()' to 
 * render an image with preset texture coordinates.
 *
 * @param[in] state The state of the library 
 * @param[in] pos The position of the image (px)
 * @param[in] color The color of the image 
 * @param[in] tex The texture to render 
 * @param[in] border_color The border color 
 * of the image (ignored if border_width <= 0).
 * @param[in] border_width The border width 
 * of the rimage
 * @param[in] corner_radius The radius to round 
 * edges with (0 => no rounded corners)
 * */
void rn_image_render_ex(
    RnState* state, 
    vec2s pos, 
    float rotation_angle,
    RnColor color, 
    RnTexture tex,
    RnColor border_color,
    float border_width, 
    float corner_radius);

/*
 * @brief Uses 'rn_image_render_ex()' to 
 * render an image with 'border_color' set to 
 * RN_NO_COLOR and 'border_width' & 'corner_radius'
 * set to zero.
 *
 * @param[in] state The state of the library 
 * @param[in] pos The position of the image (px)
 * @param[in] color The color of the image 
 * @param[in] tex The texture to render 
 * @param[in] border_color The border color 
 * of the image (ignored if border_width <= 0).
 * @param[in] border_width The border width 
 * of the rimage
 * @param[in] corner_radius The radius to round 
 * edges with (0 => no rounded corners)
 * */
void rn_image_render(
    RnState* state, 
    vec2s pos, 
    RnColor color, 
    RnTexture tex);

/*
 * @brief Uses 'rn_image_base_types()' but with base types.
 *
 * @param[in] state The state of the library 
 * @param[in] posx The X position of the rectangle (px)
 * @param[in] posy The Y position of the rectangle (px)
 * @param[in] color_r The red channel of the image 
 * @param[in] color_g The green channel of the image 
 * @param[in] color_b The blue channel of the image 
 * @param[in] color_a The alpha channel of the image 
 * */
void rn_image_render_base_types(
    RnState* state, 
    float posx, 
    float posy, 
    float rotation_angle,
    unsigned char color_r, 
    unsigned char color_g, 
    unsigned char color_b, 
    unsigned char color_a, 
    uint32_t tex_id, uint32_t tex_width, uint32_t tex_height);

uint32_t rn_utf8_to_codepoint(const char *text, uint32_t cluster, uint32_t text_length);

/*
 * @brief Renders a given text with a given 
 * font.
 *
 * This function renders the glyphs associated with 
 * the given string. Glyphs that are not yet cached 
 * will be loaded automatically by this function. 
 * This function will also add glyph information & positioning 
 * data generated by harfbuzz for the given string. 
 * 'rn_image_render_adv()' with the individual glyph's 
 * texture coordinats is used to render every glyph.
 * The given font's texture atlas is used as the texture to 
 * render glyphs with.
 *
 * @param[in] state The state of the library
 * @param[in] text The text to rener 
 * @param[in] font The font to render the text with
 * @param[in] pos The position of the text (px)
 * @param[in] color The color of the rendered text
 * @param[in] line_height The amount of pixels 
 * to travel vertically when a new line is processed.
 * (If set to 0, the font's specified line height is used.)
 * @param[in] render Whether or not to render the 
 * glyphs.
 *
 * @return The properties of the text (dimension)
 * */
RnTextProps rn_text_render_ex(
    RnState* state, 
    const char* text,
    RnFont* font, 
    vec2s pos, 
    RnColor color,
    float line_height,
    bool render);

/*
 * @brief Uses 'rn_text_render_paragraph_ex()' with 
 * render set to true.
 * (See rn_text_render_paragraph_ex documentation)
 *
 * @param[in] state The state of the library
 * @param[in] paragraph The text to rener 
 * @param[in] pos The position of the pargraph 
 * @param[in] font The font to render the paragraph with
 * @param[in] pos The position of the paragraph (px)
 * @param[in] color The color of the rendered text
 * @param[in] props The properties of the rendered paragraph (See RnParagraphProps documentation) 
 *
 * @return The properties of the text (dimension)
 * */

RnTextProps rn_text_render_paragraph(
    RnState* state, 
    const char* paragraph,
    RnFont* font, 
    vec2s pos, 
    RnColor color,
    RnParagraphProps props);

/*
 * @brief Renders a given text paragraph with a given 
 * font.
 *
 * This function renders the glyphs associated with 
 * the given string. Glyphs that are not yet cached 
 * will be loaded automatically by this function. 
 * This function will also use positioning 
 * data & glyph information (e.g ligatures) generated by harfbuzz to render 
 * glyphs.
 * 'rn_image_render_adv()' with the individual glyph's 
 * texture coordinats is used to render every glyph.
 * The given font's texture atlas is used as the texture to 
 * render glyphs with.
 *
 * The words inside the string are split by spaces and new lines. Those 
 * split words are then correctly layouted based on the pargraph's properties.
 *
 * @param[in] state The state of the library
 * @param[in] paragraph The text to rener 
 * @param[in] font The font to render the text with
 * @param[in] pos The position of the text (px)
 * @param[in] color The color of the rendered text
 * @param[in] props The properties of the rendered paragraph (See RnParagraphProps documentation) 
 * @param[in] render Whether or not to render the given text 
 *
 * @return The properties of the text (dimension)
 * */
RnTextProps rn_text_render_paragraph_ex(
    RnState* state, 
    const char* paragraph,
    RnFont* font, 
    vec2s pos, 
    RnColor color,
    RnParagraphProps props,
    bool render);

/*
 * @brief Uses 'rn_text_render_ex()' with 
 * line_height set to 0 (=> font's line height is 
 * used) and render set to true. 
 *
 * @param[in] state The state of the library
 * @param[in] text The text to rener 
 * @param[in] font The font to render the text with
 * @param[in] pos The position of the text (px)
 * @param[in] color The color of the rendered text
 *
 * @return The properties of the text (dimension)
 * */
RnTextProps rn_text_render(
    RnState* state, 
    const char* text,
    RnFont* font, 
    vec2s pos, 
    RnColor color);

/*
 * @brief Uses 'rn_text_render()' but using 
 * base types instead of structs.
 *
 * @param[in] state The state of the library
 * @param[in] text The text to rener 
 * @param[in] font The font to render the text with
 * @param[in] pos_x The X position of the text (px)
 * @param[in] pos_y The Y position of the text (px)
 * @param[in] color_r The red color-channel of the rendered text
 * @param[in] color_g The green color-channel of the rendered text
 * @param[in] color_b The blue color-channel of the rendered text
 * @param[in] color_a The alpha color-channel of the rendered text
 *
 * @return The properties of the text (dimension)
 * */

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
    );

/*
 * @brief Retrieves properties (dimensions) of a given text 
 * without rendering it.
 *
 * This function uses 'rn_text_render_ex()' with 
 * line_height set to 0 (=> font's line height is 
 * used) and render set to false. 'pos' is set to 
 * (vec2s){0, 0}. The value of 'rn_text_render_ex()'
 * is returned.
 *
 * @param[in] state The state of the library
 * @param[in] text The text to rener 
 * @param[in] font The font to render the text with
 *
 * @return The properties of the text (dimension)
 * */
RnTextProps rn_text_props(
    RnState* state, 
    const char* text, 
    RnFont* font
    );

/*
 * @brief Retrieves properties (dimensions) of a given text 
 * paragraph without rendering it.
 *
 * This function uses 'rn_text_render_paragraph_ex()' with
 * color set to LF_NO_COLOR and render set to false.
 *
 * @param[in] state The state of the library
 * @param[in] text The text to rener 
 * @param[in] pos The position of the paragraph 
 * @param[in] font The font to render the paragraph with
 * @param[in] props The properties of the paragraph (See RnParagraphProps struct documentation) 
 *
 * @return The properties of the text (dimension etc.)
 * */
RnTextProps rn_text_props_paragraph(
    RnState* state, 
    const char* text, 
    vec2s pos,
    RnFont* font,
    RnParagraphProps props
    );

/*
 * @brief Retrieves the width in pixels of a given text 
 * without rendering it. (uses rn_text_props() and return .width)
 *
 * @param[in] state The state of the library
 * @param[in] text The text to rener 
 * @param[in] font The font to render the text with
 *
 * @return The width of the text in pixels 
 * */
float rn_text_width(
    RnState* state, 
    const char* text, 
    RnFont* font
    );

/*
 * @brief Retrieves the height in pixels of a given text 
 * without rendering it. (uses rn_text_props() and return .height)
 *
 * @param[in] state The state of the library
 * @param[in] text The text to rener 
 * @param[in] font The font to render the text with
 *
 * @return The height of the text in pixels 
 * */
float rn_text_height(
    RnState* state, 
    const char* text, 
    RnFont* font
    );
/*
 * @brief Renders a given glyph by 
 * using 'rn_image_render_adv()'.
 *
 * @param[in] state The state of the library
 * @param[in] glyph The glyph to render 
 * @param[in] font The font of the glyph 
 * @param[in] pos The position of the glyph
 * @param[in] The color of the glyph
 * */
void rn_glyph_render(
    RnState* state,
    RnGlyph glyph,
    RnFont font,
    vec2s pos,
    RnColor color);

/*
 * @brief Retrieves a glyph of a given font 
 * by codepoint (used as glyph index within the font)
 *
 * This funtion either loads the glyph with the given 
 * codepoint if it is not cached yet or returns the 
 * cache glyph.
 *
 * @param[in] state The state of the library
 * @param[in] font The font to get the glyph off of.
 * @param[in] codepoint The index of the glyph to retrieve 
 *
 * @return The retrieved glyph associated with the 
 * glyph index.
 * */
RnGlyph rn_glyph_from_codepoint(
    RnState* state, 
    RnFont* font, 
    uint64_t codepoint
    );

/*
 * @brief Retrives harfbuzz positioning- & glyph-information
 * from a string with a font.
 *
 * This funtion either loads the harfbuzz data with the given 
 * string if it is not cached yet or returns the 
 * cache data.
 *
 * @param[in] state The state of the library
 * @param[in] font The font that the string is rendered with 
 * @param[in] str The string to get the harfbuzz data of 
 *
 * @return The retrieved harfbuzz data associated with the 
 * string.
 * */
RnHarfbuzzText* rn_hb_text_from_str(
    RnState* state, 
    RnFont font, 
    const char* str
    );

/*
 * @brief Sets the X coordinate from which to start culling. *
 *
 * @param[in] state The state of the library
 * @param[in] x The X coordinate from which to start culling 
 * */
void rn_set_cull_start_x(RnState* state, float x);

/*
 * @brief Sets thenY coordinate from which to start culling. *
 *
 * @param[in] state The state of the library
 * @param[in] y The Y coordinate from which to start culling 
 * */
void rn_set_cull_start_y(RnState* state, float y);

/*
 * @brief Sets the X coordinate from which to stop culling. *
 *
 * @param[in] state The state of the library
 * @param[in] x The X coordinate from which to stop culling.
 * */
void rn_set_cull_end_x(RnState* state, float x);

/*
 * @brief Sets the Y coordinate from which to stop culling. *
 *
 * @param[in] state The state of the library
 * @param[in] y The Y coordinate from which to stop culling.
 * */
void rn_set_cull_end_y(RnState* state, float y);  

/*
 * @brief Unsets the X coordinate where culling would start 
 *
 * @param[in] state The state of the library
 * */
void rn_unset_cull_start_x(RnState* state);

/*
 * @brief Unsets the Y coordinate where culling would start 
 *
 * @param[in] state The state of the library
 * */
void rn_unset_cull_start_y(RnState* state);

/*
 * @brief Unsets the X coordinate where culling would stop 
 *
 * @param[in] state The state of the library
 * */
void rn_unset_cull_end_x(RnState* state);

/*
 * @brief Unsets the Y coordinate where culling would stop 
 *
 * @param[in] state The state of the library
 * */
void rn_unset_cull_end_y(RnState* state);

/*
 * @brief Creates and returns an RGBA color from a 
 * given hex color.
 *
 * @param[in] hex The hex color to conver to RGBA 
 *
 * @return The RGBA color associated with the hex color 
 * */
RnColor rn_color_from_hex(uint32_t hex);

/*
 * @brief Returns the hex color value of a given 
 * RGBA color.
 *
 * @param[in] color The RGBA color to convert to hex 
 *
 * @return The hex value of the given color 
 * */
uint32_t rn_color_to_hex(RnColor color);

/*
 * @brief Converts a 0-1 range color to 
 * a 0-255 range color.
 * RGBA color.
 *
 * @param[in] zto The 0-1 range color 
 *
 * @return The 0-255 range color
 * */
RnColor rn_color_from_zto(vec4s zto);

/*
 * @brief Converts a 0-255 range color 
 * to 0-1 range.
 *
 * @param[in] zto The 0-255 range color 
 *
 * @return The 0-1 range color
 * */
vec4s rn_color_to_zto(RnColor color);
