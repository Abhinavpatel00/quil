#version 450
layout(push_constant) uniform PC { vec2 screen; float window; float pad; } pc;
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 color;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec2 size;
layout(location = 4) in float radius;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec2 vSize;
layout(location = 3) out float vRadius;
layout(location = 4) out float vWindow;
void main() {
    vec2 ndc = pos / pc.screen * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = color;
    vUV = uv;
    vSize = size;
    vRadius = radius;
    vWindow = pc.window;
}
