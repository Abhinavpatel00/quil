#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec4 in_params;
layout(location = 3) in vec2 in_size;
layout(location = 4) in vec2 in_uv;
layout(push_constant) uniform Push { vec2 screen; float window; float pad; } push;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_params;
layout(location = 2) out vec2 v_size;
layout(location = 3) out vec2 v_uv;
void main()
{
    gl_Position = vec4(in_pos / push.screen * 2.0 - 1.0, 0.0, 1.0);
    v_color = in_color;
    v_params = in_params;
    v_size = in_size;
    v_uv = in_uv;
}
