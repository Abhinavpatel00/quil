#version 450
layout(push_constant) uniform PC { vec2 screen; } pc;
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vColor;
void main() {
    vec2 ndc = pos / pc.screen * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = color;
}
