#include "config.h"

#include <stdio.h>
#include <unistd.h>
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
    cfg->indent_guides = true;
    cfg->vfx_modes = VFX_NONE;
    cfg->vfx_opacity = 200.0f;
    cfg->vfx_lifetime = 0.5f;
    cfg->vfx_highlight_lifetime = 0.2f;
    cfg->vfx_density = 0.7f;
    cfg->vfx_speed = 10.0f;
    cfg->vfx_phase = 1.5f;
    cfg->vfx_curl = 1.0f;
    cfg->reduced_motion = false;
    cfg->dramatic = false;
    cfg->dramatic_lead_x = 0.30f;
    cfg->dramatic_lead_y = 0.33f;
    cfg->dramatic_ms = 180u;
    cfg->background_image[0] = 0;
    cfg->background_fit = BG_FIT_COVER;
    cfg->editor_opacity = 1.0f;
    cfg->background_dim = 0.0f;
    cfg->background_vignette = 0.0f;
    cfg->window_opacity = 1.0f;
    cfg->theme[0] = 0;
    cfg->session_restore = true;
    snprintf(cfg->session_file, sizeof cfg->session_file, "%s", "~/.config/quill/session");
    cfg->auto_save_on_mode_change = true;
}

static uint32_t parse_u32(const char* s)
{
    uint32_t v = 0u;
    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    return v;
}

/* Expand a leading ~ and resolve relative paths against the directory the
 * config file lives in, then verify the result exists.
 *
 * Every path in this file is handed straight to FreeType / stb_image, neither
 * of which knows what ~ means, so `background_image = ~/hm.png` used to fail
 * with "cannot load '~/hm.png'" even though the file was there. A path was
 * also previously interpreted relative to the *current working directory*,
 * which is whatever directory the editor happened to be launched from -- so the
 * same config worked from $HOME and failed from anywhere else. Both are fixed
 * here, once, for every path-valued key.
 *
 * Returns false (and leaves the buffer empty) when the path cannot be
 * resolved, so the caller can report it instead of silently doing nothing. */
bool config_resolve_path(const char* in, const char* base_dir,
                         char* out, size_t cap)
{
    char tmp[1024];
    if (!in || !in[0] || !out || cap == 0u) return false;

    if (in[0] == '~')
    {
        const char* home = getenv("HOME");
        if (!home || !home[0]) return false;
        if (in[1] == '/' || in[1] == 0)
            snprintf(tmp, sizeof tmp, "%s%s", home, in + 1);
        else
            return false; /* ~user is not supported; be explicit, not wrong */
    }
    else if (in[0] == '/')
        snprintf(tmp, sizeof tmp, "%.*s", (int)(sizeof tmp - 1u), in);
    else if (base_dir && base_dir[0])
        snprintf(tmp, sizeof tmp, "%.*s/%.*s",
                 (int)(sizeof tmp / 2u), base_dir,
                 (int)(sizeof tmp / 2u - 2u), in);
    else
        snprintf(tmp, sizeof tmp, "%.*s", (int)(sizeof tmp - 1u), in);

    if (access(tmp, R_OK) != 0) return false;
    snprintf(out, cap, "%.*s", (int)(cap - 1u), tmp);
    return true;
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
    char base_dir[1024];
    bool opacity_seen = false;
    config_defaults(cfg);
    if (!path) return;
    base_dir[0] = 0;
    {
        /* directory of the config file, for resolving relative paths */
        const char* slash = strrchr(path, '/');
        if (slash)
            snprintf(base_dir, sizeof base_dir, "%.*s", (int)(slash - path), path);
        else
            snprintf(base_dir, sizeof base_dir, ".");
    }
    f = fopen(path, "r");
    if (!f)
    {
        /* A missing config is normal: the default "quill.conf" usually is not
         * there. Only complain when the user explicitly named a file. */
        char dflt[1024];
        snprintf(dflt, sizeof dflt, "%s", path);
        if (strcmp(dflt, "quill.conf") != 0)
            fprintf(stderr, "config: cannot open '%s' (using defaults)\n", path);
        return;
    }
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
        /* Strip an inline comment before the value is used.  The example config
         * documents every key as `key = value   # explanation`, and without
         * this the comment became part of the value: path lookups failed with
         * the whole trailing text in the filename, booleans stopped matching
         * "true", and VFX mode lists never matched a name.  A '#' only starts
         * a comment at the start of the value or after whitespace, so paths
         * that legitimately contain '#' still work. */
        {
            char* hash = v;
            while (*hash)
            {
                if (*hash == '#' && (hash == v || hash[-1] == ' ' || hash[-1] == '\t'))
                {
                    *hash = 0;
                    break;
                }
                hash++;
            }
        }
        {
            char* end = v + strlen(v);
            while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
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
        else if (strcmp(k, "indent_guides") == 0) cfg->indent_guides = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
        else if (strcmp(k, "cursor_vfx_mode") == 0 || strcmp(k, "vfx_mode") == 0) cfg->vfx_modes = vfx_parse_modes(v);
        else if (strcmp(k, "cursor_vfx_opacity") == 0 || strcmp(k, "vfx_opacity") == 0) cfg->vfx_opacity = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_lifetime") == 0 || strcmp(k, "vfx_lifetime") == 0) cfg->vfx_lifetime = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_highlight_lifetime") == 0 || strcmp(k, "vfx_highlight_lifetime") == 0) cfg->vfx_highlight_lifetime = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_density") == 0 || strcmp(k, "vfx_density") == 0) cfg->vfx_density = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_speed") == 0 || strcmp(k, "vfx_speed") == 0) cfg->vfx_speed = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_phase") == 0 || strcmp(k, "vfx_phase") == 0) cfg->vfx_phase = (float)strtod(v, NULL);
        else if (strcmp(k, "cursor_vfx_particle_curl") == 0 || strcmp(k, "vfx_curl") == 0) cfg->vfx_curl = (float)strtod(v, NULL);
        else if (strcmp(k, "reduced_motion") == 0) cfg->reduced_motion = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
        else if (strcmp(k, "dramatic") == 0) cfg->dramatic = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
        else if (strcmp(k, "dramatic_lead_x") == 0) cfg->dramatic_lead_x = clamp01((float)strtod(v, NULL) * 2.0f) * 0.5f;
        else if (strcmp(k, "dramatic_lead_y") == 0) cfg->dramatic_lead_y = clamp01((float)strtod(v, NULL) * 2.0f) * 0.5f;
        else if (strcmp(k, "dramatic_ms") == 0) cfg->dramatic_ms = parse_u32(v);
        else if (strcmp(k, "background_image") == 0)
        {
            if (strcmp(v, "none") == 0 || v[0] == 0) cfg->background_image[0] = 0;
            else if (!config_resolve_path(v, base_dir, cfg->background_image,
                                          sizeof cfg->background_image))
            {
                cfg->background_image[0] = 0;
                fprintf(stderr, "config: background_image '%s' not found "
                                "(resolved relative to '%s'); ignoring\n", v, base_dir);
            }
        }
        else if (strcmp(k, "background_fit") == 0)
        {
            if (strcmp(v, "contain") == 0) cfg->background_fit = BG_FIT_CONTAIN;
            else if (strcmp(v, "stretch") == 0) cfg->background_fit = BG_FIT_STRETCH;
            else cfg->background_fit = BG_FIT_COVER;
        }
        else if (strcmp(k, "editor_opacity") == 0)
        {
            cfg->editor_opacity = clamp01((float)strtod(v, NULL));
            opacity_seen = true;
        }
        else if (strcmp(k, "background_dim") == 0)
            cfg->background_dim = clamp01((float)strtod(v, NULL));
        else if (strcmp(k, "background_vignette") == 0)
            cfg->background_vignette = clamp01((float)strtod(v, NULL));
        else if (strcmp(k, "window_opacity") == 0)
            cfg->window_opacity = clamp01((float)strtod(v, NULL));
        else if (strcmp(k, "theme") == 0)
        {
            if (strcmp(v, "none") == 0 || strcmp(v, "default") == 0 || v[0] == 0) cfg->theme[0] = 0;
            else if (!config_resolve_path(v, base_dir, cfg->theme, sizeof cfg->theme))
            {
                cfg->theme[0] = 0;
                fprintf(stderr, "config: theme '%s' not found "
                                "(resolved relative to '%s'); ignoring\n", v, base_dir);
            }
        }
        else if (strcmp(k, "session_restore") == 0) cfg->session_restore = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
        else if (strcmp(k, "session_file") == 0) snprintf(cfg->session_file, sizeof cfg->session_file, "%s", v);
        else if (strcmp(k, "auto_save_on_mode_change") == 0) cfg->auto_save_on_mode_change = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    fclose(f);

    /* A background image at editor_opacity = 1.0 is invisible: the editor
     * background quad is drawn fully opaque over the texture. Loading it,
     * uploading it and then covering it is indistinguishable from the image not
     * loading at all, which is exactly the report this fixes. If the user asked
     * for a background but never set an opacity, reveal it the way
     * docs/APPEARANCE.md recommends. An explicit editor_opacity is respected
     * either way. */
    if (cfg->background_image[0] && !opacity_seen && cfg->editor_opacity >= 1.0f)
    {
        cfg->editor_opacity = 0.85f;
        fprintf(stderr, "config: background_image set with editor_opacity = 1.0 "
                        "(the image would be hidden); using 0.85. Set "
                        "editor_opacity explicitly to silence this.\n");
    }
}
