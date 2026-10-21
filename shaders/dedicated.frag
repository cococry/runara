#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 4) flat in int v_slot;
layout(set = 0, binding = 0) uniform sampler2D u_textures[16];
layout(location = 0) out vec4 out_color;

vec4 sample_texture(int slot, vec2 uv) {
  // Constant sampler indices keep the shader Vulkan 1.0 compatible without
  // shaderSampledImageArrayDynamicIndexing.
  switch (slot) {
    case 0: return texture(u_textures[0], uv);
    case 1: return texture(u_textures[1], uv);
    case 2: return texture(u_textures[2], uv);
    case 3: return texture(u_textures[3], uv);
    case 4: return texture(u_textures[4], uv);
    case 5: return texture(u_textures[5], uv);
    case 6: return texture(u_textures[6], uv);
    case 7: return texture(u_textures[7], uv);
    case 8: return texture(u_textures[8], uv);
    case 9: return texture(u_textures[9], uv);
    case 10: return texture(u_textures[10], uv);
    case 11: return texture(u_textures[11], uv);
    case 12: return texture(u_textures[12], uv);
    case 13: return texture(u_textures[13], uv);
    case 14: return texture(u_textures[14], uv);
    case 15: return texture(u_textures[15], uv);
    default: return vec4(1.0);
  }
}

void main() {
  out_color = sample_texture(v_slot, v_uv) * v_color;
}
