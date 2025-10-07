#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <float.h>
#include <fontconfig/fontconfig.h>
#include <cglm/mat4.h>
#include <cglm/types-struct.h>
#include <ctype.h>
#include "include/runara/runara.h"

#include <glad/glad.h>
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

static void sync_gpu_ssbo(uint32_t id, const void* data, GLsizeiptr len, uint32_t bytesize) {
  size_t bytes = len * bytesize; 
  if (bytes > 0) {
    GLint64 gpu_size = 0;
    glGetNamedBufferParameteri64v(id, GL_BUFFER_SIZE, &gpu_size);
    if (gpu_size < (GLint64)bytes) {
      glNamedBufferData(id, bytes, NULL, GL_DYNAMIC_DRAW);
    }
    glNamedBufferSubData(id, 0, bytes, data);
  }
}

static void ensure_gpu_ssbo(GLuint buf, uint32_t* cap, size_t want_bytes) {
  if (want_bytes <= *cap) return; 
  size_t new_bytes = (*cap > 0 ? *cap : 4096);
  while (new_bytes < want_bytes)
    new_bytes *= 2;
  glNamedBufferData(buf, new_bytes, NULL, GL_DYNAMIC_DRAW);
  *cap = (uint32_t)new_bytes;
}



static void vec_sync_bufs(RnState* state) {
  RnVgState* vs = &state->render.vec;

  sync_gpu_ssbo(vs->seg_ssbo, vs->segments.data, vs->segments.len, sizeof(RnSegment));
  sync_gpu_ssbo(vs->path_ssbo, vs->paths.data, vs->paths.len, sizeof(RnPathHeader)); 
  sync_gpu_ssbo(vs->paint_ssbo, vs->paints.data, vs->paints.len, sizeof(RnPaint)); 

  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, vs->seg_ssbo);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, vs->path_ssbo);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, vs->paint_ssbo);
}


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

void 
init_vector_rendering(RnState* state) {
  glCreateBuffers(1, &state->render.vec.seg_ssbo);
  glCreateBuffers(1, &state->render.vec.path_ssbo);
  glCreateBuffers(1, &state->render.vec.paint_ssbo);

  GLsizeiptr seg_bytes   = 1024 * sizeof(RnSegment);
  GLsizeiptr path_bytes  = 256  * sizeof(RnPathHeader);
  GLsizeiptr paint_bytes = 64   * sizeof(RnPaint);

  glNamedBufferData(state->render.vec.seg_ssbo,   seg_bytes,   NULL, GL_DYNAMIC_DRAW);
  glNamedBufferData(state->render.vec.path_ssbo,  path_bytes,  NULL, GL_DYNAMIC_DRAW);
  glNamedBufferData(state->render.vec.paint_ssbo, paint_bytes, NULL, GL_DYNAMIC_DRAW);

  // Bind points we’ll use in shaders: 0=Segments, 1=Paths, 2=Paints
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, state->render.vec.seg_ssbo);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, state->render.vec.path_ssbo);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, state->render.vec.paint_ssbo);

  // CPU staging init
  state->render.vec.segments = (RnVgSegmentList)DA_INIT;
  DA_RESERVE(&state->render.vec.segments, 1024);
  state->render.vec.paths = (RnVgPathHeaderList)DA_INIT;
  DA_RESERVE(&state->render.vec.paths, 256);
  state->render.vec.paints = (RnVgPaintList)DA_INIT;
  DA_RESERVE(&state->render.vec.paints, 32);
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
  state->render.vert_count = 0;
  state->render.verts = (RnVertex*)malloc(sizeof(RnVertex) * RN_MAX_RENDER_BATCH * 4);

  /* Creating vertex array & vertex buffer for the batch renderer */
  glCreateVertexArrays(1, &state->render.vao);
  glBindVertexArray(state->render.vao);

  // Creating a OpenGL vertex buffer to communicate the 
  // vertices from CPU to GPU
  glCreateBuffers(1, &state->render.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, state->render.vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(RnVertex) * RN_MAX_RENDER_BATCH * 4, NULL, 
               GL_DYNAMIC_DRAW);

  // Generate indices to index the vertices
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

  // Creating the OpenGL buffer to store indices
  glCreateBuffers(1, &state->render.ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state->render.ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, RN_MAX_RENDER_BATCH * 6 * sizeof(uint32_t), indices, GL_STATIC_DRAW);

  // Cleanup the indices CPU side
  free(indices); 

  /* Defining the vertex layout */

  // Position 
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(RnVertex), NULL);
  glEnableVertexAttribArray(0);

  // Border color
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t)offsetof(RnVertex, border_color));
  glEnableVertexAttribArray(1);

  // Border width
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t)offsetof(RnVertex, border_width));
  glEnableVertexAttribArray(2);

  // Color
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t)offsetof(RnVertex, color));
  glEnableVertexAttribArray(3);

  // Texture coordinates
  glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t*)offsetof(RnVertex, texcoord));
  glEnableVertexAttribArray(4);

  // Texture index within the batch
  glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t*)offsetof(RnVertex, tex_index));
  glEnableVertexAttribArray(5);

  // Size of the rendered shape
  glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t*)offsetof(RnVertex, size_px));
  glEnableVertexAttribArray(6);

  // Position of the rendered shape
  glVertexAttribPointer(7, 2, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t*)offsetof(RnVertex, pos_px));
  glEnableVertexAttribArray(7);

  // Corner radius 
  glVertexAttribPointer(8, 1, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t*)offsetof(RnVertex, corner_radius));
  glEnableVertexAttribArray(8);

  glVertexAttribPointer(9, 1, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t*)offsetof(RnVertex, is_text));
  glEnableVertexAttribArray(9);

  glVertexAttribPointer(10, 2, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t*)offsetof(RnVertex, min_coord));
  glEnableVertexAttribArray(10);

  glVertexAttribPointer(11, 2, GL_FLOAT, GL_FALSE, sizeof(RnVertex), 
                        (void*)(intptr_t*)offsetof(RnVertex, max_coord));
  glEnableVertexAttribArray(11);

  init_vector_rendering(state);

  /* Shader source code*/

  // Vertex shader
  const char* vert_src =
    "#version 460 core\n"
    "layout (location = 0) in vec2 a_pos;\n"
    "layout (location = 1) in vec4 a_border_color;\n"
    "layout (location = 2) in float a_border_width;\n"
    "layout (location = 3) in vec4 a_color;\n"
    "layout (location = 4) in vec2 a_texcoord;\n"
    "layout (location = 5) in float a_tex_index;\n"
    "layout (location = 6) in vec2 a_size_px;\n"
    "layout (location = 7) in vec2 a_pos_px;\n"
    "layout (location = 8) in float a_corner_radius;\n"
    "layout (location = 9) in float a_is_text;\n"
    "layout (location = 10) in vec2 a_min_coord;\n"
    "layout (location = 11) in vec2 a_max_coord;\n"

    "uniform mat4 u_proj;\n"
    "out vec4 v_border_color;\n"
    "flat out float v_border_width;\n"
    "out vec4 v_color;\n"
    "out vec2 v_texcoord;\n"
    "flat out float v_tex_index;\n"
    "flat out vec2 v_size_px;\n"
    "flat out vec2 v_pos_px;\n"
    "flat out float v_corner_radius;\n"
    "flat out float v_is_text;\n"
    "out vec2 v_min_coord;\n"
    "out vec2 v_max_coord;\n"

    "void main() {\n"
    "v_color = a_color;\n"
    "v_texcoord = a_texcoord;\n"
    "v_tex_index = a_tex_index;\n"
    "v_border_color = a_border_color;\n"
    "v_border_width = a_border_width;\n"
    "v_size_px = a_size_px;\n"
    "v_pos_px = a_pos_px;\n"
    "v_corner_radius = a_corner_radius;\n"
    "v_is_text = a_is_text;\n"
    "v_min_coord = a_min_coord;\n"
    "v_max_coord = a_max_coord;\n"
    "gl_Position = u_proj * vec4(a_pos.x, a_pos.y, 0.0f, 1.0);\n"
    "}\n";



  const char* frag_src = 
    "#version 460 core\n"
    "out vec4 o_color;\n"
    "\n"
    "in vec4 v_color;\n"
    "flat in float v_tex_index;\n"
    "in vec4 v_border_color;\n"
    "flat in float v_border_width;\n"
    "in vec2 v_texcoord;\n"
    "flat in vec2 v_size_px;\n"
    "flat in vec2 v_pos_px;\n"
    "flat in float v_corner_radius;\n"
    "flat in float v_is_text;\n"
    "uniform sampler2D u_textures[32];\n"
    "uniform vec2 u_screen_size;\n"
    "in vec2 v_min_coord;\n"
    "in vec2 v_max_coord;\n"
    "\n"
    "float rounded_box_sdf(vec2 center_pos, vec2 size, vec4 radius) {\n"
    "  radius.xy = (center_pos.x > 0.0) ? radius.xy : radius.zw;\n"
    "  radius.x = (center_pos.x > 0.0) ? radius.x : radius.y;\n"
    "\n"
    "  vec2 q = abs(center_pos) - size + radius.x;\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius.x;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "float bias = 0.5; // Small bias to prevent missing pixels\n"
    "\n"
    "if (u_screen_size.y - gl_FragCoord.y < v_min_coord.y - bias && v_min_coord.y != -1) {\n"
    "    discard;\n"
    "}\n"
    "if (u_screen_size.y - gl_FragCoord.y > v_max_coord.y + bias && v_max_coord.y != -1) {\n"
    "    discard;\n"
    "}\n"
    "if ((gl_FragCoord.x < v_min_coord.x - bias && v_min_coord.x != -1) || \n"
    "    (gl_FragCoord.x > v_max_coord.x + bias && v_max_coord.x != -1)) {\n"
    "    discard;\n"
    "}\n"
    " if(v_is_text == 1.0) {\n"
    "   vec4 sampled = texture(u_textures[int(v_tex_index)], v_texcoord);\n"
    "   o_color = sampled * v_color;\n"
    "} else {\n"
    "   vec4 display_color;"
    "   if(v_tex_index == -1) {\n"
    "     display_color = v_color;\n"
    "   } else {\n"
    "     display_color = texture(u_textures[int(v_tex_index)], v_texcoord) * v_color;\n"
    "   }\n"
    "   vec2 frag_pos = vec2(gl_FragCoord.x, u_screen_size.y - gl_FragCoord.y);\n"
    "   if(v_corner_radius != 0.0f && v_is_text != 1.0f) {\n"
    "   vec2 size_adjusted = v_size_px + v_corner_radius * 2.0f;\n"
    "   vec2 pos_adjusted = v_pos_px - v_corner_radius;\n"
    "   vec2 bottom_right = pos_adjusted + size_adjusted;\n"
    "   if (frag_pos.x < pos_adjusted.x || frag_pos.x > bottom_right.x ||\n"
    "     frag_pos.y < pos_adjusted.y || frag_pos.y > bottom_right.y) {\n"
    "       discard;\n"
    "   }\n"
    "   }\n"
    "  const vec2 rect_center = vec2(\n"
    "    v_pos_px.x + v_size_px.x / 2.0f,\n"
    "    u_screen_size.y - (v_size_px.y / 2.0f + v_pos_px.y)\n"
    "  );\n"
    "  const float edge_softness = 2.0f;\n"
    "  const float border_softness = 2.0f;\n"
    "  const vec4 corner_radius = vec4(v_corner_radius);\n"
    "\n"
    "  float shadow_softness = 0.0f;\n"
    "  vec2 shadow_offset = vec2(0.0f);\n"
    "\n"
    "  vec2 half_size = vec2(v_size_px / 2.0);\n"
    "\n"
    "  float distance = rounded_box_sdf(\n"
    "    gl_FragCoord.xy - rect_center,\n"
    "    half_size, corner_radius\n"
    "  );\n"
    "\n"
    "  float smoothed_alpha = 1.0f - smoothstep(0.0f,\n"
    "    edge_softness, distance);\n"
    "\n"
    "float border_alpha = (v_corner_radius == 0.0) ? 1.0 - step(v_border_width, abs(distance)) :\n"
    "                                               (1.0f - smoothstep(v_border_width - border_softness, v_border_width, abs(distance)));\n"


    "  float shadow_distance = rounded_box_sdf(\n"
    "    gl_FragCoord.xy - rect_center + shadow_offset,\n"
    "    half_size, corner_radius\n"
    "  );\n"
    "  float shadow_alpha = 1.0f - smoothstep(\n"
    "    -shadow_softness, shadow_softness, shadow_distance);\n"
    "\n"
    "  vec4 res_color = mix(\n"
    "    vec4(0.0f),\n"
    "    display_color,\n"
    "    min(display_color.a, smoothed_alpha)\n"
    "  );\n"
    "  if(v_border_width != 0.0f) {\n"
    "  res_color = mix(\n"
    "    res_color,\n"
    "    v_border_color,\n"
    "    min(v_border_color.a, min(border_alpha, smoothed_alpha))\n"
    "  );\n"
    "  }"
    "  o_color = res_color;\n"
    "}\n"
    "\n"
    ""
    "}\n";

  const char* comp_src =
    "#version 460 core\n"
    "layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;\n"
    "\n"
    "struct Segment { uint type; float x0,y0,x1,y1,x2,y2,x3,y3; uint flags; uint _pad; };\n"
    "layout(std430, binding = 0) readonly buffer SegBuf  { Segment   gSegments[]; };\n"
    "struct PathHeader { uint start,count,paint_fill,paint_stroke; float stroke_width,miter_limit; uint fill_rule, stroke_flags; };\n"
    "layout(std430, binding = 1) readonly buffer PathBuf { PathHeader gPaths[]; };\n"
    "struct Paint { uint type; vec4 color; vec2 p0,p1,scale,repeat; int tex_index; int _pad1; };\n"
    "layout(std430, binding = 2) readonly buffer PaintBuf { Paint gPaints[]; };\n"
    "\n"
    "const uint RN_TILE_EMPTY = 0u;\n"
    "const uint RN_TILE_FULL  = 1u;\n"
    "const uint RN_TILE_MIXED = 2u;\n"
    "struct PathTileMeta {\n"
    "    uint tiles_x, tiles_y;\n"
    "    uint tile_size;\n"
    "    uint ranges_off;\n"
    "    uint total_ranges;\n"
    "    uint built;\n"
    "};\n"
    "layout(std430, binding = 4) readonly buffer MetaBuf   { PathTileMeta gMeta[]; };\n"
    "struct TileRange { uint start, count, flags; };\n"
    "layout(std430, binding = 5) readonly buffer RangeBuf  { TileRange   gRanges[]; };\n"
    "layout(std430, binding = 6) readonly buffer IndexBuf  { uint        gIndices[]; };\n"
    "\n"
    "struct TileJob {\n"
    "    int  base_x, base_y;\n"
    "    int  rect_w, rect_h;\n"
    "    uint path_id;\n"
    "    uint tile_x, tile_y;\n"
    "    uint _pad;\n"
    "};\n"
    "layout(std430, binding = 7) readonly buffer JobBuf { TileJob gJobs[]; };\n"
    "\n"
    "layout(rgba8, binding = 0) uniform writeonly image2D uAtlas;\n"
    "\n"
    "const float EPS       = 1e-12;\n"
    "const float EPS_SMALL = 1e-6;\n"
    "\n"
    "float cross2d(vec2 a, vec2 b) { return a.x*b.y - a.y*b.x; }\n"
    "vec2 perp(vec2 v) { return vec2(-v.y, v.x); }\n"
    "\n"
    "float sd_segment(vec2 p, vec2 a, vec2 b) {\n"
    "    vec2 d = b - a;\n"
    "    float md = max(dot(d,d), EPS);\n"
    "    float t = clamp(dot(p-a,d)/md, 0.0, 1.0);\n"
    "    return length(p - (a + t*d));\n"
    "}\n"
    "\n"
    "float sd_stroke_segment_butt(vec2 p, vec2 a, vec2 b, float width, float dwedge) {\n"
    "    float radius = 0.5 * width;\n"
    "    vec2 d = b - a;\n"
    "    float len = max(length(d), EPS);\n"
    "    vec2 t = d / len, n = vec2(-t.y, t.x);\n"
    "    vec2 c = 0.5 * (a + b);\n"
    "    vec2 lp = vec2(dot(p - c, t), dot(p - c, n));\n"
    "    vec2 sdist = abs(lp) - vec2(0.5 * (len + clamp(dwedge, 0.0, width * 0.5)), radius);\n"
    "    return length(max(sdist, vec2(0))) + min(max(sdist.x, sdist.y), 0.0);\n"
    "}\n"
    "\n"
    "float sd_triangle(vec2 p, vec2 a, vec2 b, vec2 c) {\n"
    "    if (cross2d(b - a, c - a) < 0.0) { vec2 t=b; b=c; c=t; }\n"
    "    vec2 e0=b-a, e1=c-b, e2=a-c;\n"
    "    vec2 v0=p-a, v1=p-b, v2=p-c;\n"
    "    vec2 pq0=v0-e0*clamp(dot(v0,e0)/max(dot(e0,e0),EPS),0.0,1.0);\n"
    "    vec2 pq1=v1-e1*clamp(dot(v1,e1)/max(dot(e1,e1),EPS),0.0,1.0);\n"
    "    vec2 pq2=v2-e2*clamp(dot(v2,e2)/max(dot(e2,e2),EPS),0.0,1.0);\n"
    "    float d=min(min(dot(pq0,pq0),dot(pq1,pq1)),dot(pq2,pq2));\n"
    "    float s0=cross2d(e0,v0), s1=cross2d(e1,v1), s2=cross2d(e2,v2);\n"
    "    float inside=min(min(s0,s1),s2);\n"
    "    return (inside>0.0)?-sqrt(d):sqrt(d);\n"
    "}\n"
    "\n"
    "float sd_stroke_segment(vec2 p, vec2 a, vec2 b, float width) {\n"
    "    return sd_segment(p,a,b)-0.5*width;\n"
    "}\n"
    "\n"
    "vec2 line_intersect(vec2 p, vec2 r, vec2 q, vec2 s) {\n"
    "    float d = cross2d(r,s);\n"
    "    if(abs(d)<EPS_SMALL){\n"
    "        float rr=max(dot(r,r),EPS);\n"
    "        vec2 w=q-p;\n"
    "        float t=dot(w,r)/rr;\n"
    "        return p+r*t;\n"
    "    }\n"
    "    float t=cross2d(q-p,s)/d;\n"
    "    return p+r*t;\n"
    "}\n"
    "\n"
    "float distsq_line_segment(vec2 p, vec2 a, vec2 b) {\n"
    "    vec2 ab=b-a;\n"
    "    float denom=max(dot(ab,ab),EPS_SMALL);\n"
    "    float t=clamp(dot(p-a,ab)/denom,0.0,1.0);\n"
    "    vec2 q=a+t*ab;\n"
    "    vec2 d=p-q;\n"
    "    return dot(d,d);\n"
    "}\n"
    "\n"
    "void accumulate_crossing_line(vec2 p, vec2 a, vec2 b, inout int eo, inout int winding){\n"
    "    float dy=b.y-a.y;\n"
    "    if(abs(dy)<EPS_SMALL) return;\n"
    "    bool up=(a.y<=p.y)&&(b.y>p.y);\n"
    "    bool dn=(b.y<=p.y)&&(a.y>p.y);\n"
    "    if(up||dn){\n"
    "        float t=clamp((p.y-a.y)/dy,0.0,1.0);\n"
    "        float x=mix(a.x,b.x,t);\n"
    "        if(x>p.x){ eo=(eo==0)?1:0; winding+=up?+1:-1; }\n"
    "    }\n"
    "}\n"
    "\n"
    "float sd_join_miter(vec2 p, vec2 a, vec2 b, vec2 c, float width, float miter_limit){\n"
    "    vec2 d0=normalize(b-a), d1=normalize(c-b);\n"
    "    float radius=0.5*width;\n"
    "    float side=-sign(cross2d(d0,d1));\n"
    "    if(abs(side)<EPS_SMALL) return 1e9;\n"
    "    vec2 n0=perp(d0)*side, n1=perp(d1)*side;\n"
    "    const float px=0.5;\n"
    "    vec2 e0=b+n0*(radius-px), e1=b+n1*(radius-px);\n"
    "    vec2 t=line_intersect(e0,d0,e1,d1);\n"
    "    float miterlen=length(t-b)/max(radius,EPS);\n"
    "    if(miterlen>miter_limit){\n"
    "        return (side>0.0)?sd_triangle(p,b,e0,e1):sd_triangle(p,b,e1,e0);\n"
    "    } else {\n"
    "        return (side>0.0)?sd_triangle(p,e0,e1,t):sd_triangle(p,e1,e0,t);\n"
    "    }\n"
    "}\n"
    "\n"
    "vec4 do_paint(uint id, vec2 p){\n"
    "    if(id>=gPaints.length()) return vec4(0.0);\n"
    "    Paint paint=gPaints[id];\n"
    "    return paint.color;\n"
    "}\n"
    "\n"
    "bool get_neighbor_segments(uint path_start,uint path_count,uint sidx,out Segment prevSeg,out Segment nextSeg){\n"
    "    bool havePrev=false, haveNext=false;\n"
    "    if(sidx>path_start){ prevSeg=gSegments[sidx-1]; havePrev=true; }\n"
    "    if(sidx+1<path_start+path_count){ nextSeg=gSegments[sidx+1]; haveNext=true; }\n"
    "    return havePrev||haveNext;\n"
    "}\n"
    "\n"
    "vec4 rasterize_tile_px(vec2 p,uint path_id,uint tile_index){\n"
    "    const uint INVALID=0xFFFFFFFFu;\n"
    "    if(path_id==INVALID||path_id>=gPaths.length()||path_id>=gMeta.length()) return vec4(0.0);\n"
    "\n"
    "    PathHeader path=gPaths[path_id];\n"
    "    PathTileMeta meta=gMeta[path_id];\n"
    "    if(tile_index>=meta.total_ranges) return vec4(0.0);\n"
    "\n"
    "    TileRange r=gRanges[meta.ranges_off+tile_index];\n"
    "    uint begin=r.start, end=r.start+r.count;\n"
    "\n"
    "    int eo=0,winding=0;\n"
    "    bool want_miter=(path.stroke_flags&2u)!=0u;\n"
    "    float mindistsq=1e30, strokedist=1e30;\n"
    "\n"
    "    for(uint j=begin;j<end;++j){\n"
    "        if(j>=gIndices.length()) break;\n"
    "        uint sidx=gIndices[j];\n"
    "        if(sidx>=gSegments.length()) break;\n"
    "        Segment s=gSegments[sidx];\n"
    "        if(s.type!=0u) continue;\n"
    "\n"
    "        vec2 a=vec2(s.x0,s.y0), b=vec2(s.x1,s.y1);\n"
    "        accumulate_crossing_line(p,a,b,eo,winding);\n"
    "        mindistsq=min(mindistsq,distsq_line_segment(p,a,b));\n"
    "\n"
    "        if(path.stroke_width>0.0){\n"
    "            float segdist=want_miter?sd_stroke_segment_butt(p,a,b,path.stroke_width,0.0)\n"
    "                                    :sd_stroke_segment(p,a,b,path.stroke_width);\n"
    "            strokedist=min(strokedist,segdist);\n"
    "\n"
    "            if(want_miter){\n"
    "                Segment prevSeg,nextSeg;\n"
    "                bool haveNeighbors=get_neighbor_segments(path.start,path.count,sidx,prevSeg,nextSeg);\n"
    "                if(haveNeighbors){\n"
    "                    if(sidx>path.start){\n"
    "                        vec2 pa=vec2(prevSeg.x0,prevSeg.y0), pb=vec2(prevSeg.x1,prevSeg.y1);\n"
    "                        if(length(pb-a)<1e-3){\n"
    "                            float wedge=sd_join_miter(p,pa,pb,b,path.stroke_width,max(path.miter_limit,1.0));\n"
    "                            float dp=sd_stroke_segment_butt(p,pa,pb,path.stroke_width,wedge);\n"
    "                            float dn=sd_stroke_segment_butt(p,pb,b,path.stroke_width,wedge);\n"
    "                            float distSegments=min(dp,dn);\n"
    "                            float distWedge=max(wedge-0.5,-distSegments);\n"
    "                            strokedist=min(strokedist,min(distSegments,distWedge));\n"
    "                        }\n"
    "                    }\n"
    "                    if(sidx+1<path.start+path.count){\n"
    "                        vec2 nc0=vec2(nextSeg.x0,nextSeg.y0), nc1=vec2(nextSeg.x1,nextSeg.y1);\n"
    "                        if(length(b-nc0)<1e-3){\n"
    "                            float wedge=sd_join_miter(p,a,b,nc1,path.stroke_width,max(path.miter_limit,1.0));\n"
    "                            float dp=sd_stroke_segment_butt(p,a,b,path.stroke_width,wedge);\n"
    "                            float dn=sd_stroke_segment_butt(p,b,nc1,path.stroke_width,wedge);\n"
    "                            float distSegments=min(dp,dn);\n"
    "                            float distWedge=max(wedge-0.5,-distSegments);\n"
    "                            strokedist=min(strokedist,min(distSegments,distWedge));\n"
    "                        }\n"
    "                    }\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "\n"
    "    bool filled=(path.fill_rule==0u)?(eo==1):(winding!=0);\n"
    "    float edgedist=sqrt(mindistsq);\n"
    "    float signedd=edgedist*(filled?-1.0:1.0);\n"
    "    float cov=clamp(0.5-signedd/1.0,0.0,1.0);\n"
    "    float strokecov=0.0;\n"
    "    if(path.stroke_width>0.0) strokecov=clamp(0.5-(strokedist/1.0),0.0,1.0);\n"
    "\n"
    "    vec4 fill=do_paint(path.paint_fill,p);\n"
    "    vec4 stroke=(path.paint_stroke!=0xFFFFFFFFu)?do_paint(path.paint_stroke,p):vec4(0.0);\n"
    "    return fill*cov+stroke*strokecov;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    uint jobIndex = gl_WorkGroupID.x;\n"
    "    ivec2 local = ivec2(gl_LocalInvocationID.xy);\n"
    "\n"
    "    if (jobIndex >= gJobs.length()) return;\n"
    "    TileJob job = gJobs[jobIndex];\n"
    "    if (job.path_id >= gPaths.length() || job.path_id >= gMeta.length()) return;\n"
    "\n"
    "    PathTileMeta meta = gMeta[job.path_id];\n"
    "    uint tile_index = job.tile_y * meta.tiles_x + job.tile_x;\n"
    "\n"
    "\n"
    "    uint flags = gRanges[tile_index + meta.ranges_off].flags;\n"
    "    if (flags == RN_TILE_EMPTY) {\n"
    "        return;\n"
    "    }\n"
    "    else if (flags == RN_TILE_FULL) {\n"
    "        PathHeader path = gPaths[job.path_id];\n"
    "        bool want_miter = (path.stroke_flags & 2u) != 0u;\n"
    "        float offset = path.stroke_width + (want_miter ? path.miter_limit : 0.0);\n"
    "\n"
    "        ivec2 p_in_rect = ivec2(\n"
    "            int(job.tile_x) * 16 + local.x,\n"
    "            int(job.tile_y) * 16 + local.y\n"
    "        );\n"
    "        if (p_in_rect.x >= job.rect_w + int(offset) || p_in_rect.y >= job.rect_h + int(offset)) return;\n"
    "\n"
    "        vec2 p = vec2(p_in_rect) - vec2(offset * 0.5);\n"
    "        ivec2 atlas_px = ivec2(job.base_x + p_in_rect.x, job.base_y + p_in_rect.y);\n"
    "        imageStore(uAtlas, atlas_px, do_paint(path.paint_fill, p));\n"
    "    }\n"
    "    else {\n"
    "        PathHeader path = gPaths[job.path_id];\n"
    "        bool want_miter = (path.stroke_flags & 2u) != 0u;\n"
    "        float offset = path.stroke_width + (want_miter ? path.miter_limit : 0.0);\n"
    "\n"
    "        ivec2 p_in_rect = ivec2(\n"
    "            int(job.tile_x) * meta.tile_size + local.x,\n"
    "            int(job.tile_y) * meta.tile_size + local.y\n"
    "        );\n"
    "        if (p_in_rect.x >= job.rect_w + int(offset) || p_in_rect.y >= job.rect_h + int(offset)) return;\n"
    "\n"
    "        vec2 p = vec2(p_in_rect) - vec2(offset * 0.5);\n"
    "        ivec2 atlas_px = ivec2(job.base_x + p_in_rect.x, job.base_y + p_in_rect.y);\n"
    "        vec4 col = rasterize_tile_px(p, job.path_id, tile_index);\n"
    "        imageStore(uAtlas, atlas_px, col);\n"
    "    }\n"
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
  set_projection_matrix(state);
  glUniform1iv(glGetUniformLocation(state->render.shader.id, "u_textures"), RN_MAX_TEX_COUNT_BATCH, tex_slots);

  printf("AWIUDAWIUD.\n");
}

/* This function renders every vertex in the current batch */
void 
renderer_flush(RnState* state) {
  if(state->render.vert_count <= 0) return;

  // Set the data to draw (the vertices in the current batch)
  glUseProgram(state->render.shader.id);


  glBindBuffer(GL_ARRAY_BUFFER, state->render.vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(RnVertex) * state->render.vert_count, 
                  state->render.verts);

  // Bind used texture slots
  for(uint32_t i = 0; i < state->render.tex_count; i++) {
    glBindTextureUnit(i, state->render.textures[i].id);
  }

  // Upload the size of the screen to the shader
  vec2s render_size = (vec2s){(float)state->render.render_w, (float)state->render.render_h};
  glUniform2fv(glGetUniformLocation(state->render.shader.id, "u_screen_size"), 1, (float*)render_size.raw);
  glBindVertexArray(state->render.vao);

  // Commit draw call
  glDrawElements(GL_TRIANGLES, state->render.index_count, GL_UNSIGNED_INT, NULL);
  state->drawcalls++;
}

/* This function begins a new batch within the 
 * renderer 
 * */
void renderer_begin(RnState* state) {
  // Resetting all the 
  state->render.vert_count = 0;
  state->render.index_count = 0;
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
  rn_vg_sync_csr(
    &state->render.compute, 
    &state->render.compute.path_tile_metas, 
    &state->render.compute.path_tile_ranges, 
    &state->render.compute.path_tile_seg_indicies);

  RnVgTileJobList jobs = DA_INIT;
  rn_vg_collect_dirty_tile_jobs(
    state,
    &state->render.vgcacheatlas,
    &state->render.vgcache,
    &state->render.compute.path_tile_metas,
    &jobs);

  vec_sync_bufs(state);

  rn_vg_compute_update(
    &state->render.compute,
    &state->render.vgcacheatlas,
    &jobs);

  DA_FREE(&jobs);

  rn_begin_batch(state); 
}

void
rn_next_batch(RnState* state) {
  // End the current batch
  renderer_flush(state);
  // Begin a new batch
  renderer_begin(state);
}

RnVertex*
rn_add_vertex_ex(
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
  bool is_text) {
  // If the vetex does not fit into the current batch, 
  // advance to the next batch
  if(state->render.vert_count >= RN_MAX_RENDER_BATCH) {
    rn_next_batch(state);
  }

  RnVertex* vertex = &state->render.verts[state->render.vert_count];

  // Calculating the position of the vertex
  vec4 result;
  glm_mat4_mulv(transform, vert_pos.raw, result);
  state->render.verts[state->render.vert_count].pos[0] = result[0];
  state->render.verts[state->render.vert_count].pos[1] = result[1];

  vertex->pos_px[0] = pos.x; 
  vertex->pos_px[1] = pos.y;

  vertex->size_px[0] = size.x; 
  vertex->size_px[1] = size.y;

  vec4s color_zto = rn_color_to_zto(color); 

  vertex->color[0] = color_zto.r;
  vertex->color[1] = color_zto.g;
  vertex->color[2] = color_zto.b;
  vertex->color[3] = color_zto.a;
  vec4s border_color_zto = rn_color_to_zto(border_color); 

  vertex->border_color[0] = border_color_zto.r;
  vertex->border_color[1] = border_color_zto.g;
  vertex->border_color[2] = border_color_zto.b;
  vertex->border_color[3] = border_color_zto.a;

  vertex->border_width = border_width;

  vertex->corner_radius = corner_radius;

  vertex->is_text = is_text ? 1.0 : 0.0;

  vertex->texcoord[0] = texcoord.x;
  vertex->texcoord[1] = texcoord.y;

  state->render.verts[state->render.vert_count].tex_index = tex_index;

  vertex->min_coord[0] = state->cull_start.x;
  vertex->min_coord[1] = state->cull_start.y;

  vertex->max_coord[0] = state->cull_end.x;
  vertex->max_coord[1] = state->cull_end.y;

  state->render.vert_count++;

  return vertex;
}

void
rn_transform_make(vec2s pos, vec2s size, float rotation_angle, mat4* transform) {
  mat4 translate; 
  mat4 scale;
  mat4 rotation;

  vec3 pos_xyz = {pos.x, pos.y, 0.0f};
  vec3 size_xyz = {size.x, size.y, 1.0f};  
  vec3 rotation_axis = {0.0f, 0.0f, 1.0f};

  // Translating (positioning)
  glm_translate_make(translate, pos_xyz);

  // Scaling
  glm_scale_make(scale, size_xyz); 

  // Rotation
  float rad = glm_rad(rotation_angle);
  glm_rotate_make(rotation, rad, rotation_axis);

  glm_mat4_identity(*transform);

  glm_mat4_mul(translate, rotation, *transform); 
  glm_mat4_mul(*transform, scale, *transform); 
}


float rn_tex_index_from_tex(RnState* state, RnTexture tex) {
  float tex_index = -1.0f;
  for (uint32_t i = 0; i < state->render.tex_count; ++i) {
    if (tex.id == state->render.textures[i].id) {
      tex_index = i;
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

RnVertex*
rn_add_vertex(
  RnState* state, 
  vec4s vert_pos,
  mat4 transform,
  vec2s pos, 
  vec2s size, 
  RnColor color,
  RnColor border_color,
  float border_width,
  float corner_radius) {
  return rn_add_vertex_ex(state, vert_pos, transform, 
                          pos, size, color, border_color, 
                          border_width, corner_radius, 
                          (vec2s){0.0f, 0.0f}, -1.0f, false);
}

void
rn_end_batch(RnState* state) {
  renderer_flush(state);
}

void 
rn_end(RnState* state) {
  for(uint32_t i = 0; i < state->render.vgcache.len; i++) {
    RnVgCachedVectorGraphic item = state->render.vgcache.data[i];
    vec2s texcoords[4] = { 
      (vec2s){item.u0, item.v0}, 
      (vec2s){item.u1, item.v0}, 
      (vec2s){item.u1, item.v1}, 
      (vec2s){item.u0, item.v1} 
    };
    vec2s draw_pos = { item.posx, item.posy };
    RnTexture tex = { .id = state->render.vgcacheatlas.texid, .width = state->render.vgcacheatlas.w, .height = state->render.vgcacheatlas.h};
    rn_image_render(state, draw_pos, RN_WHITE, tex);  
  }

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


  pos.x = floorf(pos.x + 0.5f);
  pos.y = floorf(pos.y + 0.5f);
  size.x = floorf(size.x + 0.5f);
  size.y = floorf(size.y + 0.5f);

  vec2s pos_initial = pos;
  // Change the position in a way that the given  
  // position is taken as the top left of the shape
  pos = (vec2s){pos.x + size.x / 2.0f, pos.y + size.y / 2.0f};

  // Position is 0 if the quad has corners
  vec2s pos_matrix = {corner_radius != 0.0f ? 
    (float)state->render.render_w / 2.0f : pos.x, 
    corner_radius != 0.0f ? (float)state->render.render_h / 2.0f : pos.y};

  // Size is 0 if the quad has corners
  vec2s size_matrix = {corner_radius != 0.0f ? state->render.render_w : size.x, 
    corner_radius != 0.0f ? state->render.render_h : size.y};

  // Calculating the transform matrix
  mat4 transform;
  rn_transform_make(pos_matrix, size_matrix, rotation_angle, &transform);

  // Adding the vertices to the batch renderer
  for(uint32_t i = 0; i < 4; i++) {
    rn_add_vertex_ex(state, state->render.vert_pos[i], transform, pos_initial, size, color,
                     border_color, border_width, corner_radius,
                     (vec2s){0.0f, 0.0f}, -1, false);
  }

  // Adding the indices to the renderer
  state->render.index_count += 6;
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
  float tex_index = rn_tex_index_from_tex(state, tex);

  if (tex_index == -1.0f) {
    tex_index = (float)state->render.tex_index;
    rn_add_tex_to_batch(state, tex);
  }

  // Create transform matrix
  mat4 transform;
  rn_transform_make(pos, (vec2s){tex.width, tex.height}, rotation_angle, &transform);

  // Add vertices
  for (uint32_t i = 0; i < 4; ++i) {
    rn_add_vertex_ex(state, state->render.vert_pos[i], transform, pos_initial, 
                     (vec2s){tex.width, tex.height}, color,
                     border_color, border_width, corner_radius,
                     texcoords[i], tex_index, is_text);
  }

  // Adding the indices to the renderer
  state->render.index_count += 6;
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

bool 
rn_vg_init_atlas(RnVgCachingAtlas* atlas, uint32_t width, uint32_t height, uint32_t gutter, bool srgb) {
  if(!atlas) return false;
  memset(atlas, 0, sizeof(*atlas));

  atlas->w = width;
  atlas->h = height;
  atlas->gutter = gutter;

  glCreateTextures(GL_TEXTURE_2D, 1, &atlas->texid);
  GLenum fmt = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
  glTextureStorage2D(atlas->texid, 1, fmt, width, height);
  const uint8_t zero[4] = {0,0,0,0};
  glClearTexImage(atlas->texid, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);



  glTextureParameteri(atlas->texid, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTextureParameteri(atlas->texid, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTextureParameteri(atlas->texid, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTextureParameteri(atlas->texid, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  const float border[4] = {0,0,0,0};
  glTextureParameterfv(atlas->texid, GL_TEXTURE_BORDER_COLOR, border);

  glCreateFramebuffers(1, &atlas->fboid);
  glNamedFramebufferTexture(atlas->fboid, GL_COLOR_ATTACHMENT0, atlas->texid, 0);

  ls_atlas_init(&atlas->binpack, (ls_vec2d){.x = (uint16_t)width, .y = (uint16_t)height});

  return true;
}

void 
rn_vg_destroy_atlas(RnVgCachingAtlas* atlas) {
  if(!atlas) return;
  if(atlas->fboid) { 
    glDeleteFramebuffers(1, &atlas->fboid); 
    atlas->fboid = 0;
  }
  if(atlas->texid) { 
    glDeleteTextures(1, &atlas->texid); 
    atlas->texid = 0;
  }
  memset(atlas, 0, sizeof(*atlas));
}
void rn_vg_update_item_uvs(RnVgCachingAtlas* atlas, RnVgCachedVectorGraphic* item) {
  float gx = atlas->gutter;
  float w  = item->contentw;
  float h  = item->contenth;

  item->u0 = (item->atlasx + gx + 0.5f) / atlas->w;
  item->v0 = (item->atlasy + gx + 0.5f) / atlas->h;
  item->u1 = (item->atlasx + gx + w - 0.5f) / atlas->w;
  item->v1 = (item->atlasy + gx + h - 0.5f) / atlas->h;
}

RnVgCachedVectorGraphic rn_vg_cache_item(RnVgCachingAtlas* atlas, uint32_t w, uint32_t h, float posx, float posy, float stroke_w, float miter_limit) {
  if(!atlas) return (RnVgCachedVectorGraphic){0};

  RnVgCachedVectorGraphic item = {0};
  float packed_x, packed_y;

  ls_vec2d req = { (uint16_t)(w + 2*atlas->gutter),
    (uint16_t)(h + 2*atlas->gutter) };

  if(item.in_atlas && item.atlasw >= req.x && item.atlash >= req.y) {
    item.contentw = w; 
    item.contenth = h; 
    rn_vg_update_item_uvs(atlas, &item);
    item.dirty = true;
    return item;
  }

  ls_atlas_push_rect(&atlas->binpack, &packed_x, &packed_y, req);
  item.atlasx   = (int)packed_x;
  item.atlasy   = (int)packed_y;
  item.contentw = w;
  item.contenth = h;

  item.posx = posx; 
  item.posy = posy;
  item.stroke_w = stroke_w;

  item.bucket_id = atlas->texid;
  item.path_id = 0;
  item.dirty = true;
  item.in_atlas = true;
  rn_vg_update_item_uvs(atlas, &item);

  return item;
}

void 
rn_vg_collect_dirty(RnVgCachingAtlas* atlas, RnVgCachedVectorGraphic* items, uint32_t nitems, RnVgDirtyRectList_Compute* o_list) {
  for(uint32_t i = 0; i < nitems; i++) {
    RnVgCachedVectorGraphic* item = &items[i];
    if(!item->in_atlas || !item->dirty) continue;
    int ix = item->atlasx + atlas->gutter;
    int iy = item->atlasy + atlas->gutter;
    int iw = item->contentw + item->stroke_w + atlas->gutter + 1; 
    int ih = item->contenth + item->stroke_w + atlas->gutter + 1;

    RnVgDirtyRect_Compute r = { ix, iy, iw, ih, item->path_id, {0,0,0} };
    DA_PUSH(o_list, r);

    item->dirty = false;
  }
}
bool 
rn_vg_compute_init(RnVgState_Compute* state, const char* compute_src, uint32_t init_cap) {
  if(!state) return false;
  memset(state, 0, sizeof(*state));
  state->tile_size = 16;
  state->compute_program = create_compute_program(compute_src);
  if(!state->compute_program) return false;

  glCreateBuffers(1, &state->meta_ssbo);
  glCreateBuffers(1, &state->range_ssbo);
  glCreateBuffers(1, &state->index_ssbo);

  state->meta_cap   = 0;
  state->range_cap  = 0;
  state->index_cap  = 0;

  state->path_tile_seg_indicies = (RnUintList)DA_INIT;
  state->path_tile_ranges = (RnVgTilePathRangeList)DA_INIT;
  state->path_tile_metas = (RnVgPathTileMetaList)DA_INIT;

  state->need_shader_upload = true;

  glNamedBufferData(state->meta_ssbo,   1, NULL, GL_DYNAMIC_DRAW);
  glNamedBufferData(state->range_ssbo,  1, NULL, GL_DYNAMIC_DRAW);
  glNamedBufferData(state->index_ssbo,  1, NULL, GL_DYNAMIC_DRAW);

  glCreateBuffers(1, &state->job_ssbo);
  state->job_cap = init_cap ? init_cap : 4096; 
  glNamedBufferData(state->job_ssbo, state->job_cap*sizeof(RnVgTileJob), NULL, GL_DYNAMIC_DRAW);

  return true;
}

void 
rn_vg_compute_update(RnVgState_Compute* state, const RnVgCachingAtlas* atlas, const RnVgTileJobList* jobs) {
  if (jobs->len == 0) return;

  GLuint query;
glGenQueries(1, &query);

glBindImageTexture(0, atlas->texid, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
ensure_gpu_ssbo(state->job_ssbo, &state->job_cap, jobs->len * sizeof(RnVgTileJob));
glNamedBufferSubData(state->job_ssbo, 0, jobs->len*sizeof(RnVgTileJob), jobs->data);

glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, state->meta_ssbo);
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, state->range_ssbo);
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, state->index_ssbo);
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, state->job_ssbo);

glUseProgram(state->compute_program);

glBeginQuery(GL_TIME_ELAPSED, query);
glDispatchCompute(jobs->len, 1, 1);
glEndQuery(GL_TIME_ELAPSED);

// Wait for results (blocking, but accurate)
GLuint64 timeElapsed = 0;
glGetQueryObjectui64v(query, GL_QUERY_RESULT, &timeElapsed);
printf("Compute shader time: %f s\n", timeElapsed / 1000000000.0);
glDeleteQueries(1, &query);

}

RnAABB 
rn_vg_segment_get_aabb(RnSegment segment, float pad) {
  float minx = MIN(segment.x0, segment.x1) - pad;
  float miny = MIN(segment.y0, segment.y1) - pad;
  float maxx  = MAX(segment.x0, segment.x1) + pad;
  float maxy = MAX(segment.y0, segment.y1) + pad;

  return (RnAABB){
    .minx = minx, 
    .miny = miny, 
    .maxx = maxx, 
    .maxy =  maxy 
  };
}


static inline uint32_t clampu32_int(int v, uint32_t lo, uint32_t hi) {
    if (v < (int)lo) return lo;
    if (v > (int)hi) return hi;
    return (uint32_t)v;
}

void rn_vg_path_accumulate_rows_csr(
  RnVgState* state,
  uint32_t path_id,
  uint32_t n_tiles_x,
  uint32_t n_tiles_y,
  float path_x,
  float path_y,
  uint32_t tilesize,
  RnVgTilePathRangeList* ranges,
  RnUintList* indices)
{
  const RnPathHeader path = state->paths.data[path_id];
  const uint32_t seg_count = path.count;
  if (seg_count == 0) {
    const uint32_t total_tiles = n_tiles_x * n_tiles_y;
    for (uint32_t t = 0; t < total_tiles; ++t) {
      RnVgCSRTileRange r = { .start = indices->len, .count = 0u, .flags = RN_TILE_MIXED };
      DA_PUSH(ranges, r);
    }
    return;
  }

  const float half_stroke = path.stroke_width * 0.5f;
  const float ts = (float)tilesize;
  const float inv_ts = 1.0f / ts;
  const float eps = 1e-6f * ts;

  const uint32_t total_tiles = n_tiles_x * n_tiles_y;

  float *minx = (float*)malloc(sizeof(float) * seg_count);
  float *maxx = (float*)malloc(sizeof(float) * seg_count);
  float *miny = (float*)malloc(sizeof(float) * seg_count);
  float *maxy = (float*)malloc(sizeof(float) * seg_count);

  for (uint32_t i = 0; i < seg_count; ++i) {
    const RnSegment seg = state->segments.data[path.start + i];
    RnAABB aabb = rn_vg_segment_get_aabb(seg, half_stroke);
    aabb.miny -= half_stroke;
    minx[i] = aabb.minx;
    maxx[i] = aabb.maxx;
    miny[i] = aabb.miny;
    maxy[i] = aabb.maxy;
  }

  int  *min_tx_raw = (int*)malloc(sizeof(int) * seg_count);
  int  *max_tx_raw = (int*)malloc(sizeof(int) * seg_count);
  int  *min_ty_raw = (int*)malloc(sizeof(int) * seg_count);
  int  *max_ty_raw = (int*)malloc(sizeof(int) * seg_count);
  uint8_t *valid = (uint8_t*)malloc(seg_count); 

  for (int i = 0; i < (int)seg_count; i += 8) {
    const int remaining = (int)seg_count - i;
    const int width = remaining >= 8 ? 8 : remaining;
    __m256 v_minx = _mm256_maskload_ps(minx + i, _mm256_set_epi32(
      width>7?-1:0, width>6?-1:0, width>5?-1:0, width>4?-1:0,
      width>3?-1:0, width>2?-1:0, width>1?-1:0, width>0?-1:0));
    __m256 v_maxx = _mm256_maskload_ps(maxx + i, _mm256_set_epi32(
      width>7?-1:0, width>6?-1:0, width>5?-1:0, width>4?-1:0,
      width>3?-1:0, width>2?-1:0, width>1?-1:0, width>0?-1:0));
    __m256 v_miny = _mm256_maskload_ps(miny + i, _mm256_set_epi32(
      width>7?-1:0, width>6?-1:0, width>5?-1:0, width>4?-1:0,
      width>3?-1:0, width>2?-1:0, width>1?-1:0, width>0?-1:0));
    __m256 v_maxy = _mm256_maskload_ps(maxy + i, _mm256_set_epi32(
      width>7?-1:0, width>6?-1:0, width>5?-1:0, width>4?-1:0,
      width>3?-1:0, width>2?-1:0, width>1?-1:0, width>0?-1:0));

    const __m256 v_path_x  = _mm256_set1_ps(path_x);
    const __m256 v_path_y  = _mm256_set1_ps(path_y);
    const __m256 v_inv_ts  = _mm256_set1_ps(inv_ts);
    const __m256 v_eps     = _mm256_set1_ps(eps);

    __m256 v_minx_adj = _mm256_sub_ps(v_minx, v_eps);
    __m256 v_maxx_adj = _mm256_add_ps(v_maxx, v_eps);
    __m256 v_miny_adj = _mm256_sub_ps(v_miny, v_eps);
    __m256 v_maxy_adj = _mm256_add_ps(v_maxy, v_eps);

    __m256 v_tx_min_f = _mm256_mul_ps(_mm256_sub_ps(v_minx_adj, v_path_x), v_inv_ts);
    __m256 v_tx_max_f = _mm256_mul_ps(_mm256_sub_ps(v_maxx_adj, v_path_x), v_inv_ts);
    __m256 v_ty_min_f = _mm256_mul_ps(_mm256_sub_ps(v_miny_adj, v_path_y), v_inv_ts);
    __m256 v_ty_max_f = _mm256_mul_ps(_mm256_sub_ps(v_maxy_adj, v_path_y), v_inv_ts);

    __m256 v_tx_min_ff = _mm256_floor_ps(v_tx_min_f);
    __m256 v_tx_max_ff = _mm256_floor_ps(v_tx_max_f);
    __m256 v_ty_min_ff = _mm256_floor_ps(v_ty_min_f);
    __m256 v_ty_max_ff = _mm256_floor_ps(v_ty_max_f);

    __m256i v_tx_min_i = _mm256_cvtps_epi32(v_tx_min_ff);
    __m256i v_tx_max_i = _mm256_cvtps_epi32(v_tx_max_ff);
    __m256i v_ty_min_i = _mm256_cvtps_epi32(v_ty_min_ff);
    __m256i v_ty_max_i = _mm256_cvtps_epi32(v_ty_max_ff);

    int txmin[8], txmax[8], tymin[8], tymax[8];
    _mm256_storeu_si256((__m256i*)txmin, v_tx_min_i);
    _mm256_storeu_si256((__m256i*)txmax, v_tx_max_i);
    _mm256_storeu_si256((__m256i*)tymin, v_ty_min_i);
    _mm256_storeu_si256((__m256i*)tymax, v_ty_max_i);

    for (int k = 0; k < width; ++k) {
      const int raw_min_tx = txmin[k];
      const int raw_max_tx = txmax[k];
      const int raw_min_ty = tymin[k];
      const int raw_max_ty = tymax[k];
      const int out_x = (raw_max_tx < 0) || (raw_min_tx > (int)n_tiles_x - 1);
      const int out_y = (raw_max_ty < 0) || (raw_min_ty > (int)n_tiles_y - 1);

      if (out_x || out_y) {
        min_tx_raw[i + k] = 1; max_tx_raw[i + k] = 0;
        min_ty_raw[i + k] = 1; max_ty_raw[i + k] = 0;
        valid[i + k] = 0;
      } else {
        min_tx_raw[i + k] = raw_min_tx;
        max_tx_raw[i + k] = raw_max_tx;
        min_ty_raw[i + k] = raw_min_ty;
        max_ty_raw[i + k] = raw_max_ty;
        valid[i + k] = 1;
      }
    }
  }

  uint32_t *counts = (uint32_t*)calloc(total_tiles, sizeof(uint32_t));

  // Fixed counting pass
  for (int i = 0; i < (int)seg_count; ++i) {
    if (!valid[i]) continue;

    uint32_t min_tx = clampu32_int(min_tx_raw[i], 0u, n_tiles_x - 1);
    uint32_t max_tx = clampu32_int(max_tx_raw[i], 0u, n_tiles_x - 1);
    uint32_t min_ty = clampu32_int(min_ty_raw[i], 0u, n_tiles_y - 1);
    uint32_t max_ty = clampu32_int(max_ty_raw[i], 0u, n_tiles_y - 1);

    for (uint32_t ty = min_ty; ty <= max_ty; ++ty) {
      const uint32_t row_base = ty * n_tiles_x;
      for (uint32_t tx = min_tx; tx <= max_tx; ++tx) {
        const uint32_t tile_id = row_base + tx;
        counts[tile_id] += 1u;
      }
    }
  }

  uint32_t *offsets = (uint32_t*)malloc(sizeof(uint32_t) * (total_tiles + 1));
  offsets[0] = 0;
  for (uint32_t t = 0; t < total_tiles; ++t) {
    offsets[t + 1] = offsets[t] + counts[t];
  }
  const uint32_t total_indices = offsets[total_tiles];

  uint32_t *tmp_indices = (uint32_t*) (total_indices ? malloc(sizeof(uint32_t) * total_indices) : NULL);
  uint32_t *cursors = (uint32_t*)malloc(sizeof(uint32_t) * total_tiles);
  memcpy(cursors, offsets, sizeof(uint32_t) * total_tiles);

  // Fixed fill pass
  for (uint32_t i = 0; i < seg_count; ++i) {
    if (!valid[i]) continue;

    uint32_t min_tx = clampu32_int(min_tx_raw[i], 0u, n_tiles_x - 1);
    uint32_t max_tx = clampu32_int(max_tx_raw[i], 0u, n_tiles_x - 1);
    uint32_t min_ty = clampu32_int(min_ty_raw[i], 0u, n_tiles_y - 1);
    uint32_t max_ty = clampu32_int(max_ty_raw[i], 0u, n_tiles_y - 1);

    const uint32_t seg_index = path.start + i;
    for (uint32_t ty = min_ty; ty <= max_ty; ++ty) {
      const uint32_t row_base = ty * n_tiles_x;
      for (uint32_t tx = min_tx; tx <= max_tx; ++tx) {
        const uint32_t tile_id = row_base + tx;
        const uint32_t dst = cursors[tile_id]++;
        tmp_indices[dst] = seg_index;
      }
    }
  }

  for (uint32_t ty = 0; ty < n_tiles_y; ++ty) {
    for (uint32_t tx = 0; tx < n_tiles_x; ++tx) {
      const uint32_t tile_id = ty * n_tiles_x + tx;

      const uint32_t start = indices->len;
      const uint32_t cnt   = counts[tile_id];
      const uint32_t off   = offsets[tile_id];

      for (uint32_t j = 0; j < cnt; ++j) {
        DA_PUSH(indices, tmp_indices[off + j]);
      }
      const uint32_t end = indices->len;

      RnVgCSRTileRange r = {
        .start = start,
        .count = end - start,
        .flags = RN_TILE_MIXED
      };
      DA_PUSH(ranges, r);
    }
  }

  free(minx); free(maxx); free(miny); free(maxy);
  free(min_tx_raw); free(max_tx_raw); free(min_ty_raw); free(max_ty_raw);
  free(valid);
  free(counts);
  free(offsets);
  free(cursors);
  if (tmp_indices) free(tmp_indices);
}

void rn_vg_path_build_tiles(
    RnVgState* state, 
    uint32_t path_id,
    uint32_t tilesize,
    RnVgPathTileMetaList* metas, 
    RnVgTilePathRangeList* ranges,
    RnUintList* indices)
{
    RnPathHeader path = state->paths.data[path_id];

    // Fix precedence bug: pad now correct
    float pad = ceilf(path.stroke_width * 0.5f +
                     ((path.stroke_flags & RN_STROKE_JOIN_MITER) ? path.miter_limit : 0.0f));

    // Compute path AABB
    RnAABB path_aabb = { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (uint32_t i = 0; i < path.count; i++) {
        RnSegment seg = state->segments.data[path.start + i];
        RnAABB seg_aabb = rn_vg_segment_get_aabb(seg, pad);
        path_aabb.minx = MIN(path_aabb.minx, seg_aabb.minx);
        path_aabb.miny = MIN(path_aabb.miny, seg_aabb.miny);
        path_aabb.maxx = MAX(path_aabb.maxx, seg_aabb.maxx);
        path_aabb.maxy = MAX(path_aabb.maxy, seg_aabb.maxy);
    }

    // Degenerate path
    if (path_aabb.minx > path_aabb.maxx || path_aabb.miny > path_aabb.maxy) {
        RnVgPathTileMeta meta = {0};
        meta.tiles_x = meta.tiles_y = 0;
        meta.built = true;
        meta.tile_size = tilesize;
        DA_INSERT(metas, path_id, meta);
        return;
    }

    uint32_t n_tiles_x = (uint32_t)((path_aabb.maxx - path_aabb.minx + tilesize - 1) / tilesize);
    uint32_t n_tiles_y = (uint32_t)((path_aabb.maxy - path_aabb.miny + tilesize - 1) / tilesize);

    // Reserve CSR space
    uint32_t total_tiles = n_tiles_x * n_tiles_y;
    DA_RESERVE(ranges, ranges->len + total_tiles);

    // Fill per-tile CSR
    size_t base_range = ranges->len;
    size_t base_index = indices->len;

    rn_vg_path_accumulate_rows_csr(
        state, path_id, n_tiles_x, n_tiles_y,
        path_aabb.minx, path_aabb.miny, tilesize,
        ranges, indices
    );

    // Meta entry for this path
    RnVgPathTileMeta meta = {0};
    meta.ranges_off   = (uint32_t)base_range;
    meta.tiles_x      = n_tiles_x;
    meta.tiles_y      = n_tiles_y;
    meta.tile_size    = tilesize;
    meta.total_ranges = total_tiles;
    meta.built        = true;

    DA_INSERT(metas, path_id, meta);
}




void rn_vg_collect_dirty_tile_jobs(
  RnState* state,
  const RnVgCachingAtlas* atlas, 
  RnVgCachedVectorGraphicList* items,
  const RnVgPathTileMetaList* metas, 
  RnVgTileJobList* o_jobs) {
  if(!o_jobs) return;
  DA_CLEAR(o_jobs);
  for(uint32_t i = 0; i < items->len; i++) {
    RnVgCachedVectorGraphic* item = &items->data[i];
    if(!item->in_atlas || !item->dirty) continue;
    const RnVgPathTileMeta meta = metas->data[item->path_id];
    if(!meta.built || meta.tiles_x == 0 || meta.tiles_y == 0) continue;

    int ix = item->atlasx + atlas->gutter;
    int iy = item->atlasy + atlas->gutter;
    int iw = (int)meta.tiles_x * (int)meta.tile_size;
    int ih = (int)meta.tiles_y * (int)meta.tile_size;
    for (uint32_t y = 0; y < meta.tiles_y; y++) {
      for (uint32_t x = 0; x < meta.tiles_x; x++) {
        const RnVgCSRTileRange r = state->render.compute.path_tile_ranges.data[meta.ranges_off + y*meta.tiles_x + x];
        if (r.flags == RN_TILE_EMPTY) continue; 
        RnVgTileJob job = { 
          .base_x = ix, .base_y = iy, 
          .rect_w = iw, .rect_h = ih, 
          .path_id = item->path_id, 
          .tile_x =  x, .tile_y = y,
          ._pad = 0 // ingore
        };
        DA_PUSH(o_jobs, job);
      }
    }
    item->dirty = false;
  }
}

void rn_vg_sync_csr(RnVgState_Compute* state,
                    const RnVgPathTileMetaList* metas_cpu,
                    const RnVgTilePathRangeList* ranges_cpu,
                    const RnUintList* indices_cpu) {
  if (!state->need_shader_upload) return; 

  size_t meta_count  = metas_cpu->len;
  size_t range_count = ranges_cpu->len;
  size_t index_count = indices_cpu->len;

  size_t meta_bytes  = meta_count  * sizeof(RnVgPathTileMeta_GPU);
  size_t range_bytes = range_count * sizeof(RnVgCSRTileRange);
  size_t index_bytes = index_count * sizeof(uint32_t);

  RnVgPathTileMeta_GPU* metas_gpu = NULL;
  if (meta_count > 0) {
    metas_gpu = malloc(meta_bytes);
    for (uint32_t i = 0; i < meta_count; i++) {
      RnVgPathTileMeta m = metas_cpu->data[i];
      metas_gpu[i] = (RnVgPathTileMeta_GPU){
        m.tiles_x, m.tiles_y, m.tile_size,
        m.ranges_off, 
        m.total_ranges,
        index_count,
        m.built ? 1u : 0u
      };
    }
  }
  ensure_gpu_ssbo(state->meta_ssbo,  &state->meta_cap,  meta_bytes);
  ensure_gpu_ssbo(state->range_ssbo, &state->range_cap, range_bytes);
  ensure_gpu_ssbo(state->index_ssbo, &state->index_cap, index_bytes);

  if (meta_bytes  > 0) glNamedBufferSubData(state->meta_ssbo,  0, meta_bytes,  metas_gpu);
  if (range_bytes > 0) glNamedBufferSubData(state->range_ssbo, 0, range_bytes, ranges_cpu->data);
  if (index_bytes > 0) glNamedBufferSubData(state->index_ssbo, 0, index_bytes, indices_cpu->data);

  free(metas_gpu);
  state->need_shader_upload = false;
}

