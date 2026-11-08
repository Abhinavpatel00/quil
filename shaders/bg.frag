#version 450
/* Optional editor background image. Drawn once, fullscreen, before the
 * chrome. uv_scale/uv_offset implement cover/contain/stretch; opacity is
 * the image's own opacity. Outside the image (contain letterbox) we emit
 * transparent so the theme clear color shows through.
 *
 * dim and vignette are the wallpaper readability controls, and window is the
 * whole-window opacity (premultiplied output, blend is ONE /
 * ONE_MINUS_SRC_ALPHA). */
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC {
    vec2  uv_scale;
    vec2  uv_offset;
    float opacity;
    float dim;       /* 0 = untouched, 1 = black */
    float vignette;  /* 0 = flat, 1 = edges darkened */
    float window;    /* 1 = opaque window */
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
    /* Dim and vignette are applied in linear space (the sRGB view returns
     * linear samples), so darkening keeps its perceived contrast. The
     * vignette ramps from 45% of the half-diagonal to the corners. */
    float v = length((vUV - 0.5) * 2.0) * 0.70710678;
    float shade = (1.0 - pc.dim) * (1.0 - pc.vignette * smoothstep(0.45, 1.0, v));
    float a = c.a * pc.opacity * pc.window;
    outColor = vec4(c.rgb * shade * a, a);
}
