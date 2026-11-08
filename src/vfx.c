#include "vfx.h"

#define VFX_PI 3.14159265358979323846f
#define MU_TWO_PI   6.28318530717958647692f
#define MU_HALF_PI  1.57079632679489661923f
#define MU_INV_TWO_PI 0.15915494309189533577f

// Fast approximate sine and cosine.
// Input must be in [-PI, PI].
// No libm dependencies.

static inline float
mu_sin_poly(float x)
{
    float x2 = x * x;

    return x * (1.0f + x2 *
        (-0.1666666716f + x2 *
        (0.0083333477f + x2 *
        (-0.0001984090f + x2 * 0.0000027526f))));
}

static inline float
mu_cos_poly(float x)
{
    float x2 = x * x;

    return 1.0f + x2 *
        (-0.5f + x2 *
        (0.0416666418f + x2 *
        (-0.0013888397f + x2 * 0.0000247609f)));
}

static inline void
mu_sincosf(float x, float *s, float *c)
{
    // Fast quadrant reduction.
    // Input is assumed to be in [-PI, PI].

    float sign_c = 1.0f;

    if (x > MU_HALF_PI) {
        x = MU_PI - x;
        sign_c = -1.0f;
    } else if (x < -MU_HALF_PI) {
        x = -MU_PI - x;
        sign_c = -1.0f;
    }

    *s = mu_sin_poly(x);
    *c = sign_c * mu_cos_poly(x);
}

static inline float
mu_sinf(float x)
{
    float s, c;
    mu_sincosf(x, &s, &c);
    return s;
}

static inline float
mu_cosf(float x)
{
    float s, c;
    mu_sincosf(x, &s, &c);
    return c;
}


static const uint32_t TRAIL_BITS[VFX_TRAIL_COUNT] = {
    VFX_RAILGUN, VFX_TORPEDO, VFX_PIXIEDUST, VFX_COMET, VFX_EMBER, VFX_AURORA, VFX_GLITCH,
    VFX_SNOW, VFX_CONFETTI, VFX_SPARK, VFX_SMOKE
};
static const uint32_t HL_BITS[VFX_HL_COUNT] = {
    VFX_SONICBOOM, VFX_RIPPLE, VFX_WIREFRAME, VFX_PULSE, VFX_SHOCKWAVE, VFX_HALO
};

static const char* MODE_NAMES[] = {
    "railgun", "torpedo", "pixiedust", "sonicboom", "ripple", "wireframe",
    "comet", "ember", "aurora", "glitch", "pulse",
    "snow", "confetti", "spark", "smoke", "shockwave", "halo"
};
static const uint32_t MODE_BITS[] = {
    VFX_RAILGUN, VFX_TORPEDO, VFX_PIXIEDUST, VFX_SONICBOOM, VFX_RIPPLE, VFX_WIREFRAME,
    VFX_COMET, VFX_EMBER, VFX_AURORA, VFX_GLITCH, VFX_PULSE,
    VFX_SNOW, VFX_CONFETTI, VFX_SPARK, VFX_SMOKE, VFX_SHOCKWAVE, VFX_HALO
};

uint32_t vfx_parse_modes(const char* s)
{
    char buf[256];
    char* tok;
    char* save;
    uint32_t modes = 0u;
    uint32_t i;
    if (!s) return 0u;
    snprintf(buf, sizeof buf, "%s", s);
    for (tok = strtok_r(buf, ",; \t", &save); tok; tok = strtok_r(NULL, ",; \t", &save))
    {
        if (strcmp(tok, "none") == 0 || strcmp(tok, "off") == 0 || tok[0] == 0)
            continue;
        for (i = 0u; i < sizeof MODE_BITS / sizeof MODE_BITS[0]; ++i)
            if (strcmp(tok, MODE_NAMES[i]) == 0)
            {
                modes |= MODE_BITS[i];
                break;
            }
    }
    return modes;
}

const char* vfx_mode_name(uint32_t bit)
{
    uint32_t i;
    for (i = 0u; i < sizeof MODE_BITS / sizeof MODE_BITS[0]; ++i)
        if (MODE_BITS[i] == bit) return MODE_NAMES[i];
    return "none";
}

static uint32_t rng_next(uint64_t* state)
{
    uint64_t old = *state;
    uint32_t xorshifted, rot;
    *state = old * 6364136223846793005ULL + 1442695040888963407ULL;
    xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31u));
}

static float rng_f(uint64_t* state)
{
    return (float)(rng_next(state) >> 8) * (1.0f / 16777216.0f);
}

static void rng_dir(uint64_t* state, float* x, float* y)
{
    float dx = rng_f(state) * 2.0f - 1.0f;
    float dy = rng_f(state) * 2.0f - 1.0f;
    float l = quill_sqrtf(dx * dx + dy * dy);
    if (l < 1e-6f) { *x = 1.0f; *y = 0.0f; return; }
    *x = dx / l;
    *y = dy / l;
}

void vfx_init(VfxState* v)
{
    uint32_t i;
    memset(v, 0, sizeof *v);
    for (i = 0u; i < VFX_TRAIL_COUNT; ++i)
        v->trail[i].rng = 0x853C49E6748FEA9BULL + i * 0x9E3779B97F4A7C15ULL;
}

void vfx_reset(VfxState* v)
{
    uint32_t i;
    for (i = 0u; i < VFX_TRAIL_COUNT; ++i)
    {
        v->trail[i].count = 0u;
        v->trail[i].remainder = 0.0f;
        v->trail[i].prev_valid = false;
    }
    for (i = 0u; i < VFX_HL_COUNT; ++i)
        v->hl[i].active = false;
    v->dest_valid = false;
}

static void pool_push(VfxPool* p, float x, float y, float vx, float vy,
                      float rot, float life, float size,
                      float r, float g, float b)
{
    uint32_t i;
    if (p->count >= QUILL_VFX_POOL) return;
    i = p->count++;
    p->x[i] = x; p->y[i] = y;
    p->vx[i] = vx; p->vy[i] = vy;
    p->rot[i] = rot; p->life[i] = life;
    p->size[i] = size;
    p->cr[i] = r; p->cg[i] = g; p->cb[i] = b;
}

static void color_lin(uint32_t rgba, float out[4])
{
    quill_color_linear(rgba, out);
}

/* --- Neovide trail spawn ports ----------------------------------------- */

static void spawn_railgun(VfxPool* p, const MotionConfig* cfg,
                          float px, float py, float dx, float dy,
                          float dist, float t, float h, const float base[4])
{
    float phase = t / VFX_PI * cfg->vfx_phase * (dist / h);
    float vx = mu_sinf(phase) * 2.0f * cfg->vfx_speed;
    float vy = mu_cosf(phase) * 2.0f * cfg->vfx_speed;
    (void)dx; (void)dy;
    pool_push(p, px + dx * t, py + dy * t, vx, vy,
              VFX_PI * cfg->vfx_curl, t * cfg->vfx_lifetime, 1.0f,
              base[0], base[1], base[2]);
}

static void spawn_torpedo(VfxPool* p, const MotionConfig* cfg,
                          float px, float py, float dx, float dy,
                          float dist, float t, float h, const float base[4])
{
    float rx, ry, l, inv;
    float tx = 0.0f, ty = 0.0f;
    (void)dist; (void)t;
    if (dist > 1e-6f) { tx = dx / dist; ty = dy / dist; }
    rng_dir(&p->rng, &rx, &ry);
    rx -= tx * 1.5f;
    ry -= ty * 1.5f;
    l = quill_sqrtf(rx * rx + ry * ry);
    inv = l > 1e-6f ? 1.0f / l : 0.0f;
    pool_push(p, px + dx * rng_f(&p->rng), py + dy * rng_f(&p->rng) + h * 0.5f,
              rx * inv * cfg->vfx_speed, ry * inv * cfg->vfx_speed,
              (rng_f(&p->rng) - 0.5f) * (VFX_PI * 0.5f) * cfg->vfx_curl,
              t * cfg->vfx_lifetime, 1.0f, base[0], base[1], base[2]);
}

static void spawn_pixiedust(VfxPool* p, const MotionConfig* cfg,
                            float px, float py, float dx, float dy,
                            float dist, float t, float h, const float base[4])
{
    float rx, ry;
    (void)dist;
    rng_dir(&p->rng, &rx, &ry);
    pool_push(p, px + dx * rng_f(&p->rng), py + dy * rng_f(&p->rng) + h * 0.5f,
              rx * 0.5f * 3.0f * cfg->vfx_speed,
              (0.4f + quill_fabsf(ry)) * 3.0f * cfg->vfx_speed,
              (rng_f(&p->rng) - 0.5f) * (VFX_PI * 0.5f) * cfg->vfx_curl,
              t * cfg->vfx_lifetime, 1.0f, base[0], base[1], base[2]);
}

/* --- Quill extra trail modes ------------------------------------------- */

static void spawn_comet(VfxPool* p, const MotionConfig* cfg,
                        float px, float py, float dx, float dy,
                        float dist, float t, float h, const float base[4])
{
    float tx = 0.0f, ty = 0.0f, c[4];
    if (dist > 1e-6f) { tx = dx / dist; ty = dy / dist; }
    color_lin(0xB4E0FFFFu, c);
    (void)base; (void)h;
    pool_push(p, px + dx * t, py + dy * t,
              -tx * cfg->vfx_speed * 1.5f + (rng_f(&p->rng) - 0.5f) * 4.0f,
              -ty * cfg->vfx_speed * 1.5f + (rng_f(&p->rng) - 0.5f) * 4.0f,
              0.0f, t * cfg->vfx_lifetime, 1.0f, c[0], c[1], c[2]);
}

static void spawn_ember(VfxPool* p, const MotionConfig* cfg,
                        float px, float py, float dx, float dy,
                        float dist, float t, float h, const float base[4])
{
    float c[4];
    (void)dist; (void)base;
    color_lin(0xFAB387FFu, c);
    pool_push(p, px + dx * rng_f(&p->rng), py + dy * rng_f(&p->rng) + h * 0.5f,
              (rng_f(&p->rng) - 0.5f) * cfg->vfx_speed,
              -(0.5f + rng_f(&p->rng)) * cfg->vfx_speed * 2.0f,
              (rng_f(&p->rng) - 0.5f) * 2.0f,
              t * cfg->vfx_lifetime, 1.0f, c[0], c[1], c[2]);
}

static void spawn_aurora(VfxPool* p, const MotionConfig* cfg,
                         float px, float py, float dx, float dy,
                         float dist, float t, float h, const float base[4])
{
    float c[4];
    (void)dist; (void)base; (void)h;
    color_lin(0xA6E3A1FFu, c);
    pool_push(p, px + dx * t, py + dy * t,
              (rng_f(&p->rng) - 0.5f) * cfg->vfx_speed * 0.5f,
              -cfg->vfx_speed * 0.3f,
              2.0f + rng_f(&p->rng) * 3.0f,
              t * cfg->vfx_lifetime, 1.0f, c[0], c[1], c[2]);
}

static void spawn_glitch(VfxPool* p, const MotionConfig* cfg,
                         float px, float py, float dx, float dy,
                         float dist, float t, float h, const float base[4])
{
    float c[4];
    uint32_t pick = rng_next(&p->rng) % 3u;
    (void)dist; (void)base; (void)h;
    color_lin(pick == 0u ? 0xF38BA8FFu : (pick == 1u ? 0x89DCEBFFu : 0xF9E2AFFFu), c);
    pool_push(p, px + dx * rng_f(&p->rng), py + dy * rng_f(&p->rng),
              (rng_f(&p->rng) - 0.5f) * cfg->vfx_speed * 6.0f,
              (rng_f(&p->rng) - 0.5f) * cfg->vfx_speed,
              0.0f, t * cfg->vfx_lifetime * 0.6f, 1.0f, c[0], c[1], c[2]);
}

static void spawn_snow(VfxPool* p, const MotionConfig* cfg,
                       float px, float py, float dx, float dy,
                       float dist, float t, float h, const float base[4])
{
    float c[4];
    (void)dist; (void)base;
    color_lin(0xE6EDF3FFu, c);
    pool_push(p, px + dx * rng_f(&p->rng), py + dy * rng_f(&p->rng) + h * 0.5f,
              (rng_f(&p->rng) - 0.5f) * cfg->vfx_speed * 0.4f,
              (0.3f + rng_f(&p->rng) * 0.3f) * cfg->vfx_speed,
              rng_f(&p->rng) * 6.2831853f,
              t * cfg->vfx_lifetime * 1.3f, 0.6f + rng_f(&p->rng) * 0.7f,
              c[0], c[1], c[2]);
}

static void spawn_confetti(VfxPool* p, const MotionConfig* cfg,
                           float px, float py, float dx, float dy,
                           float dist, float t, float h, const float base[4])
{
    static const uint32_t CONF[5] = {
        0xF38BA8FFu, 0xA6E3A1FFu, 0x89B4FAFFu, 0xF9E2AFFFu, 0xCBA6F7FFu
    };
    float c[4];
    (void)dist; (void)base;
    color_lin(CONF[rng_next(&p->rng) % 5u], c);
    pool_push(p, px + dx * rng_f(&p->rng), py + dy * rng_f(&p->rng) + h * 0.5f,
              (rng_f(&p->rng) - 0.5f) * cfg->vfx_speed * 2.5f,
              -(0.3f + rng_f(&p->rng)) * cfg->vfx_speed * 1.2f,
              rng_f(&p->rng) * 6.2831853f,
              t * cfg->vfx_lifetime * 1.4f, 0.7f + rng_f(&p->rng) * 0.6f,
              c[0], c[1], c[2]);
}

static void spawn_spark(VfxPool* p, const MotionConfig* cfg,
                        float px, float py, float dx, float dy,
                        float dist, float t, float h, const float base[4])
{
    float rx, ry, c[4], sp;
    (void)dist; (void)base; (void)h;
    color_lin(0xFFF6C2FFu, c);
    rng_dir(&p->rng, &rx, &ry);
    sp = (2.5f + rng_f(&p->rng) * 2.0f) * cfg->vfx_speed;
    pool_push(p, px + dx * t, py + dy * t, rx * sp, ry * sp,
              0.0f, t * cfg->vfx_lifetime * 0.5f, 0.5f + rng_f(&p->rng) * 0.5f,
              c[0], c[1], c[2]);
}

static void spawn_smoke(VfxPool* p, const MotionConfig* cfg,
                        float px, float py, float dx, float dy,
                        float dist, float t, float h, const float base[4])
{
    float c[4];
    (void)dist; (void)base;
    color_lin(0x7F849CFFu, c);
    pool_push(p, px + dx * rng_f(&p->rng), py + dy * rng_f(&p->rng) + h * 0.5f,
              (rng_f(&p->rng) - 0.5f) * cfg->vfx_speed * 0.4f,
              -(0.3f + rng_f(&p->rng) * 0.4f) * cfg->vfx_speed * 0.5f,
              rng_f(&p->rng) * 6.2831853f,
              t * cfg->vfx_lifetime * 1.6f, 0.8f + rng_f(&p->rng) * 0.6f,
              c[0], c[1], c[2]);
}

typedef void (*SpawnFn)(VfxPool*, const MotionConfig*, float, float, float, float,
                        float, float, float, const float[4]);
static const SpawnFn SPAWN[VFX_TRAIL_COUNT] = {
    spawn_railgun, spawn_torpedo, spawn_pixiedust,
    spawn_comet, spawn_ember, spawn_aurora, spawn_glitch,
    spawn_snow, spawn_confetti, spawn_spark, spawn_smoke
};

#define INTEGRATE(...) \
    do { \
        uint32_t i; \
        for (i = 0u; i < p->count; ++i) \
        { \
            p->x[i] += p->vx[i] * dt; \
            p->y[i] += p->vy[i] * dt; \
            __VA_ARGS__; \
        } \
    } while (0)

static void update_rotate(VfxPool* p, float dt)
{
    INTEGRATE({
        float a = p->rot[i] * dt;
        float s = mu_sinf(a), c = mu_cosf(a);
        float vx = p->vx[i] * c - p->vy[i] * s;
        float vy = p->vx[i] * s + p->vy[i] * c;
        p->vx[i] = vx; p->vy[i] = vy;
    });
}

static void update_comet(VfxPool* p, float dt)
{
    INTEGRATE({
        p->vx[i] *= 0.98f;
        p->vy[i] *= 0.98f;
    });
}

static void update_ember(VfxPool* p, float dt)
{
    INTEGRATE({
        p->vy[i] -= 30.0f * dt;
        p->vx[i] += mu_sinf(p->life[i] * 12.0f + p->rot[i]) * 8.0f * dt;
    });
}

static void update_aurora(VfxPool* p, float dt)
{
    INTEGRATE({
        p->vx[i] += mu_sinf(p->life[i] * p->rot[i]) * 24.0f * dt;
        p->vy[i] *= 0.99f;
    });
}

static void update_glitch(VfxPool* p, float dt)
{
    INTEGRATE({
        p->x[i] += mu_sinf(p->life[i] * 80.0f) * 12.0f * dt;
        p->vx[i] *= 0.90f;
    });
}

static void update_snow(VfxPool* p, float dt)
{
    INTEGRATE({
        p->vx[i] = mu_sinf(p->life[i] * 2.0f + p->rot[i]) * 10.0f;
    });
}

static void update_confetti(VfxPool* p, float dt)
{
    INTEGRATE({
        p->vy[i] += 60.0f * dt;
        p->vx[i] += mu_sinf(p->life[i] * 6.0f + p->rot[i]) * 20.0f * dt;
    });
}

static void update_spark(VfxPool* p, float dt)
{
    INTEGRATE({
        p->vy[i] += 80.0f * dt;
        p->vx[i] *= 0.96f;
    });
}

static void update_smoke(VfxPool* p, float dt)
{
    INTEGRATE({
        p->vy[i] *= 0.98f;
        p->vx[i] += mu_sinf(p->life[i] + p->rot[i]) * 4.0f * dt;
    });
}

typedef void (*UpdateFn)(VfxPool*, float);
static const UpdateFn UPDATE[VFX_TRAIL_COUNT] = {
    update_rotate, update_rotate, update_rotate,
    update_comet, update_ember, update_aurora, update_glitch,
    update_snow, update_confetti, update_spark, update_smoke
};

static void pool_update(VfxPool* p, uint32_t mode, const MotionConfig* cfg,
                        float dt, float dest_x, float dest_y,
                        float h, bool immediate, const float base[4])
{
    uint32_t i;
    float dx, dy, dist, fcount;
    uint32_t count, spawn_cap;

    i = 0u;
    while (i < p->count)
    {
        p->life[i] -= dt;
        if (p->life[i] <= 0.0f)
        {
            uint32_t last = --p->count;
            p->x[i] = p->x[last]; p->y[i] = p->y[last];
            p->vx[i] = p->vx[last]; p->vy[i] = p->vy[last];
            p->rot[i] = p->rot[last]; p->life[i] = p->life[last];
            p->size[i] = p->size[last];
            p->cr[i] = p->cr[last]; p->cg[i] = p->cg[last]; p->cb[i] = p->cb[last];
        }
        else
            ++i;
    }

    UPDATE[mode](p, dt);

    if (!p->prev_valid)
    {
        p->prev_x = dest_x;
        p->prev_y = dest_y;
        p->prev_valid = true;
        return;
    }
    dx = dest_x - p->prev_x;
    dy = dest_y - p->prev_y;
    dist = quill_sqrtf(dx * dx + dy * dy);
    if (dist > 0.0f && !immediate && h > 0.0f)
    {
        fcount = (dist / h) * cfg->vfx_density + p->remainder;
        count = (uint32_t)fcount;
        p->remainder = fcount - (float)count;
        spawn_cap = QUILL_VFX_POOL - p->count;
        if (count > spawn_cap) count = spawn_cap;
        if (count > 256u) count = 256u;
        for (i = 0u; i < count; ++i)
        {
            float t = (float)(i + 1u) / (float)count;
            SPAWN[mode](p, cfg, p->prev_x, p->prev_y, dx, dy, dist, t, h, base);
        }
    }
    p->prev_x = dest_x;
    p->prev_y = dest_y;
}

bool vfx_update(VfxState* v, const MotionConfig* cfg, float dt,
                float dest_x, float dest_y, float w, float h,
                bool immediate, const float base_lin[4])
{
    uint32_t i;
    bool changed;
    if (!v || !cfg || cfg->vfx_modes == VFX_NONE || cfg->reduced_motion)
        return false;
    changed = !v->dest_valid || quill_fabsf(dest_x - v->dest_x) > 0.5f || quill_fabsf(dest_y - v->dest_y) > 0.5f;
    for (i = 0u; i < VFX_HL_COUNT; ++i)
    {
        if ((cfg->vfx_modes & HL_BITS[i]) == 0u) continue;
        if (changed)
        {
            v->hl[i].cx = dest_x + w * 0.5f;
            v->hl[i].cy = dest_y + h * 0.5f;
            v->hl[i].t = 0.0f;
            v->hl[i].active = true;
        }
        if (v->hl[i].active)
        {
            float life = cfg->vfx_highlight_lifetime > 0.0f ? cfg->vfx_highlight_lifetime : cfg->vfx_lifetime;
            v->hl[i].t += dt / life;
            if (v->hl[i].t >= 1.0f) v->hl[i].active = false;
        }
    }
    for (i = 0u; i < VFX_TRAIL_COUNT; ++i)
    {
        if ((cfg->vfx_modes & TRAIL_BITS[i]) == 0u) continue;
        pool_update(&v->trail[i], i, cfg, dt, dest_x, dest_y, h, immediate, base_lin);
    }
    v->dest_x = dest_x;
    v->dest_y = dest_y;
    v->dest_valid = true;
    return vfx_active(v);
}

bool vfx_active(const VfxState* v)
{
    uint32_t i;
    for (i = 0u; i < VFX_TRAIL_COUNT; ++i)
        if (v->trail[i].count > 0u) return true;
    for (i = 0u; i < VFX_HL_COUNT; ++i)
        if (v->hl[i].active) return true;
    return false;
}

static void emit_vfx(DrawList* dl, float x, float y, float w, float h,
                     float r, float g, float b, float a,
                     float shape, float stroke)
{
    VfxQuad* q;
    if (w <= 0.0f || h <= 0.0f || a <= 0.0f) return;
    if (dl->nvfx >= QUILL_MAX_VFX) { dl->overflow++; return; }
    q = &dl->vfx[dl->nvfx++];
    q->x = x; q->y = y; q->w = w; q->h = h;
    q->r = r; q->g = g; q->b = b; q->a = a;
    q->shape = shape; q->stroke = stroke; q->soft = 1.0f; q->pad = 0.0f;
}

void vfx_render(const VfxState* v, const MotionConfig* cfg, DrawList* dl,
                float font_w, float font_h)
{
    uint32_t m, i;
    float opacity = cfg->vfx_opacity / 255.0f;
    if (!v || !cfg || cfg->vfx_modes == VFX_NONE) return;

    for (m = 0u; m < VFX_TRAIL_COUNT; ++m)
    {
        const VfxPool* p = &v->trail[m];
        float shape, stroke, base_size;
        if ((cfg->vfx_modes & TRAIL_BITS[m]) == 0u || p->count == 0u) continue;
        switch (m)
        {
        case 0: case 1: shape = 1.0f; stroke = font_h * 0.2f; base_size = font_w * 0.5f; break;
        case 2: shape = 3.0f; stroke = 0.0f; base_size = font_w * 0.2f; break;
        case 3: shape = 0.0f; stroke = 0.0f; base_size = font_w * 0.4f; break;
        case 4: shape = 3.0f; stroke = 0.0f; base_size = font_w * 0.18f; break;
        case 5: shape = 0.0f; stroke = 0.0f; base_size = font_w * 0.3f; break;
        case 7: shape = 0.0f; stroke = 0.0f; base_size = font_w * 0.22f; break;  /* snow */
        case 8: shape = 3.0f; stroke = 0.0f; base_size = font_w * 0.30f; break;  /* confetti */
        case 9: shape = 0.0f; stroke = 0.0f; base_size = font_w * 0.28f; break;  /* spark */
        case 10: shape = 0.0f; stroke = 0.0f; base_size = font_w * 0.50f; break; /* smoke */
        default: shape = 3.0f; stroke = 0.0f; base_size = font_w * 0.5f; break;
        }
        for (i = 0u; i < p->count; ++i)
        {
            float ratio = cfg->vfx_lifetime > 0.0f ? p->life[i] / cfg->vfx_lifetime : 0.0f;
            float size = base_size * (m == 2u ? 1.0f : ratio) * p->size[i];
            float alpha = ratio * opacity;
            float w = size, h = size;
            if (m == 5u) h = font_h * 0.8f * ratio;
            if (m == 6u) { w = font_w * 0.6f * ratio; h = font_h * 0.2f; }
            if (m == 7u) { size = base_size * (0.6f + 0.4f * ratio) * p->size[i]; w = size; h = size; }
            if (m == 8u) { size = base_size * p->size[i]; w = size * 0.7f; h = size; }
            if (m == 10u) { size = base_size * (1.0f + (1.0f - ratio) * 1.6f) * p->size[i]; w = size; h = size; alpha = ratio * opacity * 0.35f; }
            emit_vfx(dl, p->x[i] - w * 0.5f, p->y[i] - h * 0.5f, w, h,
                     p->cr[i], p->cg[i], p->cb[i], alpha, shape, stroke);
        }
    }

    for (i = 0u; i < VFX_HL_COUNT; ++i)
    {
        const VfxHighlight* hl = &v->hl[i];
        float t, size, alpha, w, h;
        if ((cfg->vfx_modes & HL_BITS[i]) == 0u || !hl->active) continue;
        t = hl->t;
        alpha = (1.0f - t * t) * opacity;
        size = t * 3.0f * font_h;
        switch (i)
        {
        case 0: /* sonicboom: filled oval */
            emit_vfx(dl, hl->cx - size * 0.5f, hl->cy - size * 0.5f, size, size,
                     0.8f, 0.85f, 1.0f, alpha, 0.0f, 0.0f);
            break;
        case 1: /* ripple: stroked oval */
            emit_vfx(dl, hl->cx - size * 0.5f, hl->cy - size * 0.5f, size, size,
                     0.8f, 0.85f, 1.0f, alpha, 1.0f, font_h * 0.2f);
            break;
        case 2: /* wireframe: stroked rect */
            emit_vfx(dl, hl->cx - size * 0.5f, hl->cy - size * 0.5f, size, size,
                     0.8f, 0.85f, 1.0f, alpha, 2.0f, font_h * 0.2f);
            break;
        case 3: /* pulse: filled rect */
            w = font_w * 2.0f + t * font_w * 4.0f; h = font_h + t * font_h * 2.0f;
            emit_vfx(dl, hl->cx - w * 0.5f, hl->cy - h * 0.5f, w, h,
                     0.8f, 0.85f, 1.0f, alpha, 3.0f, 0.0f);
            break;
        case 4: /* shockwave: double expanding ring */
            size = t * 4.0f * font_h;
            emit_vfx(dl, hl->cx - size * 0.5f, hl->cy - size * 0.5f, size, size,
                     0.8f, 0.85f, 1.0f, alpha, 1.0f, font_h * 0.25f);
            w = size * 0.6f;
            emit_vfx(dl, hl->cx - w * 0.5f, hl->cy - w * 0.5f, w, w,
                     0.8f, 0.85f, 1.0f, alpha * 0.6f, 1.0f, font_h * 0.2f);
            break;
        default: /* halo: soft thick ring, gentle linear fade */
            size = (0.6f + t * 1.8f) * font_h * 2.0f;
            alpha = (1.0f - t) * opacity * 0.5f;
            emit_vfx(dl, hl->cx - size * 0.5f, hl->cy - size * 0.5f, size, size,
                     0.8f, 0.85f, 1.0f, alpha, 1.0f, font_h * 0.35f);
            break;
        }
    }
}
