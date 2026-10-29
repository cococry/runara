#version 450

#ifndef RN_TEXTURE_SLOTS
#define RN_TEXTURE_SLOTS 16
#endif

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 4) flat in int v_slot;
layout(set = 0, binding = 0) uniform sampler2D u_textures[RN_TEXTURE_SLOTS];
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform YuvMetadata {
  layout(offset = 8) uint conversion[RN_TEXTURE_SLOTS];
} yuv;

vec4 convert_yuv(vec4 color, uint info) {
  if ((info & 1u) == 0u) return color;
  uint model = (info >> 1u) & 3u;
  vec2 coefficients = model == 1u ? vec2(0.2126, 0.0722)
                    : model == 2u ? vec2(0.2627, 0.0593)
                                  : vec2(0.299, 0.114);
  float kr = coefficients.x, kb = coefficients.y;
  uint depth = info >> 4u;
  if (depth == 12u) color *= 65535.0 / (4095.0 * 16.0);
  float maximum = exp2(float(depth)) - 1.0;
  float scale = exp2(float(depth - 8u));
  float luma = color.g;
  vec2 chroma = color.br - vec2(128.0 * scale / maximum);
  if ((info & 8u) == 0u) {
    luma = (luma - 16.0 * scale / maximum) * maximum / (219.0 * scale);
    chroma *= maximum / (224.0 * scale);
  }
  vec3 rgb = vec3(luma) + vec3(
      2.0 * (1.0 - kr) * chroma.y,
      -2.0 * (kb * (1.0 - kb) * chroma.x + kr * (1.0 - kr) * chroma.y) / (1.0 - kr - kb),
      2.0 * (1.0 - kb) * chroma.x);
  return vec4(clamp(rgb, 0.0, 1.0), color.a);
}

vec4 sample_texture(int slot, vec2 uv) {
  /* constant sampler indices keep the shader Vulkan 1.0 compatible without
   shaderSampledImageArrayDynamicIndexing */

  switch (slot) {
    case 0: return texture(u_textures[0], uv);
    case 1: return texture(u_textures[1], uv);
    case 2: return texture(u_textures[2], uv);
    case 3: return texture(u_textures[3], uv);
#if RN_TEXTURE_SLOTS > 4
    case 4: return texture(u_textures[4], uv);
#endif
#if RN_TEXTURE_SLOTS > 5
    case 5: return texture(u_textures[5], uv);
#endif
#if RN_TEXTURE_SLOTS > 6
    case 6: return texture(u_textures[6], uv);
#endif
#if RN_TEXTURE_SLOTS > 7
    case 7: return texture(u_textures[7], uv);
#endif
#if RN_TEXTURE_SLOTS > 8
    case 8: return texture(u_textures[8], uv);
#endif
#if RN_TEXTURE_SLOTS > 9
    case 9: return texture(u_textures[9], uv);
#endif
#if RN_TEXTURE_SLOTS > 10
    case 10: return texture(u_textures[10], uv);
#endif
#if RN_TEXTURE_SLOTS > 11
    case 11: return texture(u_textures[11], uv);
#endif
#if RN_TEXTURE_SLOTS > 12
    case 12: return texture(u_textures[12], uv);
#endif
#if RN_TEXTURE_SLOTS > 13
    case 13: return texture(u_textures[13], uv);
#endif
#if RN_TEXTURE_SLOTS > 14
    case 14: return texture(u_textures[14], uv);
#endif
#if RN_TEXTURE_SLOTS > 15
    case 15: return texture(u_textures[15], uv);
#endif
    default: return vec4(1.0);
  }
}

void main() {
  out_color = convert_yuv(sample_texture(v_slot, v_uv), yuv.conversion[v_slot]) * v_color;
}
