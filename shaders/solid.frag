#version 450
/* Flat quads plus an anti-aliased rounded-rect path. radius = 0 keeps the
 * original pixel-exact coverage (text highlights, cursor, gutter): those quads
 * abut each other and must not be blended at the seams.
 *
 * Output is premultiplied (rgb and alpha both carry the coverage and the
 * window opacity) because the blend state is ONE / ONE_MINUS_SRC_ALPHA. For an
 * opaque window this renders exactly like the previous straight-alpha path. */
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec2 vSize;
layout(location = 3) in float vRadius;
layout(location = 4) in float vWindow;
layout(location = 0) out vec4 outColor;

void main()
{
    float alpha = 1.0;
    if (vRadius > 0.5)
    {
        vec2 half_size = max(vSize * 0.5, vec2(0.001));
        float r = min(vRadius, min(half_size.x, half_size.y));
        vec2 p = (vUV - 0.5) * vSize;
        vec2 q = abs(p) - (half_size - vec2(r));
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        alpha = 1.0 - smoothstep(-0.75, 0.75, d);
    }
    float a = vColor.a * alpha * vWindow;
    outColor = vec4(vColor.rgb * a, a);
}
