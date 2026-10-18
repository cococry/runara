#version 450

layout(location = 0) in vec2 a_local_pos;
layout(location = 1) in vec2 a_local_uv;
layout(location = 2) in vec4 i_rect;
layout(location = 3) in vec4 i_color;
layout(location = 4) in vec4 i_uv;
layout(location = 5) in vec4 i_params;

layout(push_constant) uniform Constants { vec2 u_screen_size; } push_data;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out int v_layer;
layout(location = 3) flat out int v_kind;
layout(location = 4) flat out int v_slot;

void main() {
  vec2 position = i_rect.xy + a_local_pos * i_rect.zw;
  // A positive Vulkan viewport height maps NDC -1 to the top of the target.
  vec2 ndc = position / push_data.u_screen_size * 2.0 - 1.0;
  gl_Position = vec4(ndc, 0.0, 1.0);
  v_uv = mix(i_uv.xy, i_uv.zw, a_local_uv);
  v_color = i_color;
  v_layer = int(i_params.z + 0.5);
  v_kind = int(i_params.w + 0.5);
  v_slot = int(i_params.x + 0.5);
}
