#include "config.h"
#include "vfx.h"

void config_defaults(MotionConfig* cfg)
{
    cfg->style = MOTION_EASE;
    cfg->scroll_ms = 300u;
    cfg->cursor_ms = 150u;
    cfg->blink_ms = 530u;
    cfg->picker_ms = 120u;
    cfg->scroll_far_lines = 1u;
    cfg->trail_size = 1.0f;
    cfg->smooth_blink = true;
    cfg->animate_insert = true;
    cfg->vfx_modes = VFX_NONE;
    cfg->vfx_opacity = 200.0f;
    cfg->vfx_lifetime = 0.5f;
    cfg->vfx_highlight_lifetime = 0.2f;
    cfg->vfx_density = 0.7f;
    cfg->vfx_speed = 10.0f;
    cfg->vfx_phase = 1.5f;
    cfg->vfx_curl = 1.0f;
    cfg->reduced_motion = false;
    cfg->background_image[0] = 0;
    cfg->background_fit = BG_FIT_COVER;
    cfg->editor_opacity = 1.0f;
    cfg->theme[0] = 0;
    cfg->session_restore = true;
    snprintf(cfg->session_file, sizeof cfg->session_file, "%s", "~/.config/quill/session");
}

static uint32_t parse_u32(const char* s)
{
    uint32_t v = 0u;
    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    return v;
}

static float clamp01(float v)
{
    if (!(v > 0.0f)) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

void config_load_file(MotionConfig* cfg, const char* path)
{
    FILE* f;
    char line[1024];
    config_defaults(cfg);
    if (!path) return;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f))
    {
        char* eq = strchr(line, '=');
        char* k = line;
        char* v;
        if (!eq) continue;
        *eq = 0;
        v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        {
            char* end = k + strlen(k);
            while (end > k && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
                *--end = 0;
        }
        while (*v == ' ' || *v == '\t') v++;
        {
            char* end = v + strlen(v);
            while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
                *--end = 0;
        }
        if (strcmp(k, "style") == 0)
        {
            if (strcmp(v, "linear") == 0) cfg->style = MOTION_LINEAR;
            else if (strcmp(v, "instant") == 0) cfg->style = MOTION_INSTANT;
            else cfg->style = MOTION_EASE;
        }
        else if (strcmp(k, "scroll_ms") == 0) cfg->scroll_ms = parse_u32(v);
        else if (strcmp(k, "cursor_ms") == 0) cfg->cursor_ms = parse_u32(v);
        else if (strcmp(k, "blink_ms") == 0) cfg->blink_ms = parse_u32(v);
        else if (strcmp(k, "picker_ms") == 0) cfg->picker_ms = parse_u32(v);
        else if (strcmp(k, "scroll_far_lines") == 0) cfg->scroll_far_lines = parse_u32(v);
        else if (strcmp(k, "trail_size") == 0) cfg->trail_size = (float)strtod(v, NULL);
        else if (strcmp(k, "smooth_blink") == 0) cfg->smooth_blink = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
        else if (strcmp(k, "animate_insert") == 0) cfg->animate_insert = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
        else if (strcmp(k, "cursor_vfx_mode") == 0 || strcmp(k, "vfx_mode") == 0) cfg->vfx_modes = vfx_parse_modes(v);
        else if (strcmp(k, "cursor_vfx_opacity") == 0 || strcmp(k, "vfx_opacity") == 0) cfg->vfx_opacity = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_lifetime") == 0 || strcmp(k, "vfx_lifetime") == 0) cfg->vfx_lifetime = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_highlight_lifetime") == 0 || strcmp(k, "vfx_highlight_lifetime") == 0) cfg->vfx_highlight_lifetime = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_density") == 0 || strcmp(k, "vfx_density") == 0) cfg->vfx_density = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_speed") == 0 || strcmp(k, "vfx_speed") == 0) cfg->vfx_speed = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_phase") == 0 || strcmp(k, "vfx_phase") == 0) cfg->vfx_phase = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_curl") == 0 || strcmp(k, "vfx_curl") == 0) cfg->vfx_curl = (float)strtod(v, NULL);
        else if (strcmp(k, "reduced_motion") == 0) cfg->reduced_motion = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
        else if (strcmp(k, "background_image") == 0)
        {
            if (strcmp(v, "none") == 0 || v[0] == 0) cfg->background_image[0] = 0;
            else snprintf(cfg->background_image, sizeof cfg->background_image, "%s", v);
        }
        else if (strcmp(k, "background_fit") == 0)
        {
            if (strcmp(v, "contain") == 0) cfg->background_fit = BG_FIT_CONTAIN;
            else if (strcmp(v, "stretch") == 0) cfg->background_fit = BG_FIT_STRETCH;
            else cfg->background_fit = BG_FIT_COVER;
        }
        else if (strcmp(k, "editor_opacity") == 0) cfg->editor_opacity = clamp01((float)strtod(v, NULL));
        else if (strcmp(k, "theme") == 0)
        {
            if (strcmp(v, "none") == 0 || strcmp(v, "default") == 0 || v[0] == 0) cfg->theme[0] = 0;
            else snprintf(cfg->theme, sizeof cfg->theme, "%s", v);
        }
        else if (strcmp(k, "session_restore") == 0) cfg->session_restore = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
        else if (strcmp(k, "session_file") == 0) snprintf(cfg->session_file, sizeof cfg->session_file, "%s", v);
    }
    fclose(f);
}
