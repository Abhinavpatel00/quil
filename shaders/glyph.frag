#version 450
layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(push_constant) uniform PC { vec2 screen; float gamma; float contrast; float window; float pad; } pc;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() {
    float cov = texture(atlas, vUV).r;
    float a = clamp(cov + pc.contrast * cov * (1.0 - cov), 0.0, 1.0);
    a = pow(a, pc.gamma);
    /* Premultiplied output: the blend state is ONE / ONE_MINUS_SRC_ALPHA. */
    float A = vColor.a * a * pc.window;
    outColor = vec4(vColor.rgb * A, A);
}
