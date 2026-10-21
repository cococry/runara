#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in int v_layer;
layout(location = 3) flat in int v_kind;
layout(set = 0, binding = 0) uniform sampler2DArray u_texture_array;
layout(location = 0) out vec4 out_color;

void main() {
  if (v_kind == 0) {
    out_color = v_color;
    return;
  }
  vec4 texel = texture(u_texture_array, vec3(v_uv, float(v_layer)));
  if (v_kind == 1)
    out_color = vec4(v_color.rgb, v_color.a * texel.a);
  else
    out_color = texel * v_color;
}
