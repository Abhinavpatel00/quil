#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_params;
layout(location = 2) in vec2 v_size;
layout(location = 3) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float rect_sdf(vec2 p, vec2 b)
{
    vec2 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

void main()
{
    vec2 b = max(v_size * 0.5, vec2(0.001));
    vec2 p = (v_uv - 0.5) * v_size;
    float shape = v_params.x;
    float stroke = v_params.y;
    float aa = max(v_params.z, 0.5);
    float d;
    float alpha;
    if (shape < 0.5)
    {
        d = (length(p / b) - 1.0) * min(b.x, b.y);
        alpha = 1.0 - smoothstep(-aa, aa, d);
    }
    else if (shape < 1.5)
    {
        d = (length(p / b) - 1.0) * min(b.x, b.y);
        alpha = 1.0 - smoothstep(-aa, aa, abs(d) - stroke * 0.5);
    }
    else if (shape < 2.5)
    {
        d = rect_sdf(p, b);
        alpha = 1.0 - smoothstep(-aa, aa, abs(d) - stroke * 0.5);
    }
    else
    {
        d = rect_sdf(p, b);
        alpha = 1.0 - smoothstep(-aa, aa, d);
    }
    out_color = vec4(v_color.rgb, v_color.a * alpha);
}
