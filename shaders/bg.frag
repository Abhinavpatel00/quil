#version 450
/* Optional editor background image. Drawn once, fullscreen, before the
 * chrome. uv_scale/uv_offset implement cover/contain/stretch; opacity is
 * the image's own opacity. Outside the image (contain letterbox) we emit
 * transparent so the theme clear color shows through. */
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC {
    vec2  uv_scale;
    vec2  uv_offset;
    float opacity;
    float pad;
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 uv = vUV * pc.uv_scale + pc.uv_offset;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        outColor = vec4(0.0);
        return;
    }
    vec4 c = texture(tex, uv);
    outColor = vec4(c.rgb, c.a * pc.opacity);
}
