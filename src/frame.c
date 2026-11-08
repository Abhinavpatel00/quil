#include "frame.h"
#include "vfx.h"
#include "search.h"
#include "theme.h"

/* QUILL_THEME (the active palette) is defined in theme.c and can be replaced
 * at runtime by a theme file; the built-in default is Noir. */

static void emit_solid(DrawList* dl, float x, float y, float w, float h,
                       const float c[4])
{
    SolidQuad* q;
    if (w <= 0.0f || h <= 0.0f) return;
    if (dl->nsolids >= QUILL_MAX_SOLIDS) { dl->overflow++; return; }
    q = &dl->solids[dl->nsolids++];
    q->x = x; q->y = y; q->w = w; q->h = h;
    q->r = c[0]; q->g = c[1]; q->b = c[2]; q->a = c[3];
}

static void emit_glyph(DrawList* dl, float x, float y, const AtlasEntry* e,
                       const float c[4], uint32_t atlas_w, uint32_t atlas_h)
{
    GlyphQuad* q;
    if (!e || e->w == 0u || e->h == 0u) return;
    if (dl->nglyphs >= QUILL_MAX_GLYPHS) { dl->overflow++; return; }
    q = &dl->glyphs[dl->nglyphs++];
    q->x = x; q->y = y; q->w = (float)e->w; q->h = (float)e->h;
    q->u0 = (float)e->x / (float)atlas_w;
    q->v0 = (float)e->y / (float)atlas_h;
    q->u1 = (float)(e->x + e->w) / (float)atlas_w;
    q->v1 = (float)(e->y + e->h) / (float)atlas_h;
    q->r = c[0]; q->g = c[1]; q->b = c[2]; q->a = c[3];
}

static void emit_shaped(DrawList* dl, FontSystem* fs, const ShapedGlyph* g,
                        uint32_t n, float x, float baseline_y,
                        const uint8_t* color, uint32_t color_len,
                        const float lin[16][4], float clip_x0, float clip_x1,
                        float scroll_x)
{
    uint32_t i;
    uint32_t aw = font_atlas_width(fs), ah = font_atlas_height(fs);
    for (i = 0u; i < n; ++i)
    {
        const AtlasEntry* e;
        float gx, gy;
        uint8_t ci;
        uint32_t phase;
        float frac;
        e = NULL;
        frac = (x + g[i].x_offset - scroll_x) - quill_floorf(x + g[i].x_offset - scroll_x);
        phase = (uint32_t)(frac * 4.0f) % 4u;
        e = font_atlas_get(fs, g[i].face, g[i].glyph, phase);
        if (!e) continue;
        if (e->w <= 1u && e->h <= 1u && e->left == 0 && e->top == 0)
            continue; /* zero-ink space */
        gx = quill_floorf(x + g[i].x_offset - scroll_x) + (float)e->left;
        gy = baseline_y - (float)e->top - g[i].y_offset;
        if (gx + (float)e->w < clip_x0 || gx > clip_x1)
            continue;
        ci = g[i].cluster < color_len ? color[g[i].cluster] : QC_DEFAULT;
        if (ci >= 16u) ci = QC_DEFAULT;
        emit_glyph(dl, gx, gy, e, lin[ci], aw, ah);
    }
}

void frame_build(Document* d, FontSystem* fs, const FrameParams* p,
                 DrawList* dl, FrameScratch* scratch,
                 LineLayout* layouts, uint32_t* nlayouts)
{
    float lin[16][4];
    float bg[4], fg[4], gut_bg[4], gut_fg[4], line_hl[4], sel[4], cur[4];
    float st_bg[4], st_fg[4];
    float lh, base, adv, pad, gutter, text_x, status_h, text_h, op;
    uint32_t rows, i, digits, nlines, cursor_line, cursor_col;
    uint32_t nsp = 0u;
    mu_quill_view view;
    mu_quill_cursor cc;
    uint32_t nl = 0u;
    uint32_t aw, ah;
    SelectionSet fallback;
    const SelectionSet* ss = p->selections;
    const SelCursor* prim;

    dl->nsolids = 0u;
    dl->nglyphs = 0u;
    dl->overflow = 0u;
    *nlayouts = 0u;

    for (i = 0u; i < 16u; ++i)
        quill_color_linear(QUILL_THEME.syntax[i], lin[i]);
    quill_color_linear(QUILL_THEME.bg, bg);
    quill_color_linear(QUILL_THEME.fg, fg);
    quill_color_linear(QUILL_THEME.gutter_bg, gut_bg);
    quill_color_linear(QUILL_THEME.gutter_fg, gut_fg);
    quill_color_linear(QUILL_THEME.line_hl, line_hl);
    quill_color_linear(QUILL_THEME.selection, sel);
    quill_color_linear(QUILL_THEME.cursor, cur);
    quill_color_linear(QUILL_THEME.status_bg, st_bg);
    quill_color_linear(QUILL_THEME.status_fg, st_fg);
    (void)fg;

    /* Editor opacity fades only the chrome (background, gutter, highlights,
     * selections, status bar) so text/cursor/VFX stay readable. The default
     * 1.0 skips this entirely, keeping the unconfigured path unchanged. */
    op = p->motion ? p->motion->editor_opacity : 1.0f;
    if (op < 0.0f) op = 0.0f;
    if (op > 1.0f) op = 1.0f;
    if (op < 1.0f)
    {
        bg[3] *= op;
        gut_bg[3] *= op;
        line_hl[3] *= op;
        sel[3] *= op;
        st_bg[3] *= op;
    }

    lh = font_line_height(fs);
    base = font_baseline(fs);
    adv = font_advance(fs);
    pad = 8.0f;
    status_h = lh + 2.0f * pad;
    text_h = (float)p->height > status_h ? (float)p->height - status_h : 0.0f;
    rows = (uint32_t)(text_h / lh) + 1u;
    if (rows > QUILL_MAX_VISIBLE) rows = QUILL_MAX_VISIBLE;
    nlines = d->q.nlines ? d->q.nlines : 1u;
    digits = 1u;
    {
        uint32_t t = nlines;
        while (t >= 10u) { t /= 10u; digits++; }
    }
    gutter = p->line_numbers ? (float)digits * adv + 2.0f * pad : 0.0f;
    text_x = gutter + pad;
    aw = font_atlas_width(fs);
    ah = font_atlas_height(fs);

    emit_solid(dl, 0.0f, 0.0f, (float)p->width, (float)p->height, bg);
    if (p->line_numbers)
        emit_solid(dl, 0.0f, 0.0f, gutter, text_h, gut_bg);

    if (!ss)
    {
        ss_init(&fallback);
        fallback.c[0].anchor = d->anchor;
        fallback.c[0].head = d->cursor;
        ss = &fallback;
    }
    prim = &ss->c[ss->primary < ss->count ? ss->primary : 0u];
    cc = mu_quill_offset_to_cursor(&d->q, prim->head);
    cursor_line = cc.line;
    cursor_col = cc.col;

    if (cursor_line >= p->top_line && cursor_line < p->top_line + rows)
    {
        float y = (float)(cursor_line - p->top_line) * lh - p->scroll_y_offset;
        emit_solid(dl, 0.0f, y, (float)p->width, lh, line_hl);
    }

    if (d->syntax && d->q.nlines != 0u)
    {
        uint32_t start_off = mu_quill_line_offset(&d->q, p->top_line);
        uint32_t end_line = p->top_line + rows;
        uint32_t end_off;
        if (end_line >= nlines) end_line = nlines - 1u;
        end_off = (end_line + 1u < nlines)
            ? mu_quill_line_offset(&d->q, end_line + 1u)
            : d->q.text_len;
        nsp = syntax_highlight(d->syntax, &d->q, start_off, end_off,
                               scratch->spans, QUILL_MAX_SPANS);
    }
    mu_quill_view_start(&d->q, p->top_line, &view);
    for (i = 0u; i < rows; ++i)
    {
        mu_quill_line l;
        uint32_t len, k, ng;
        const ShapedGlyph* g;
        float y = (float)i * lh - p->scroll_y_offset;
        float baseline_y = y + base;
        LineLayout* ll;

        if (!mu_quill_view_next_text(&d->q, &view, (char*)scratch->line,
                                     QUILL_MAX_LINE, &l))
            break;
        len = l.len < QUILL_MAX_LINE ? l.len : QUILL_MAX_LINE;
        memset(scratch->color, QC_DEFAULT, len);
        for (k = 0u; k < nsp; ++k)
        {
            uint32_t a, b, j;
            if (scratch->spans[k].end <= l.offset ||
                scratch->spans[k].start >= l.offset + len)
                continue;
            a = scratch->spans[k].start > l.offset ? scratch->spans[k].start - l.offset : 0u;
            b = scratch->spans[k].end < l.offset + len ? scratch->spans[k].end - l.offset : len;
            if (a > len) a = len;
            if (b > len) b = len;
            for (j = a; j < b; ++j)
                scratch->color[j] = scratch->spans[k].color;
        }
        ng = font_shape_line(fs, scratch->line, len, p->tab_width, &g);

        if (nl < QUILL_MAX_VISIBLE)
        {
            ll = &layouts[nl++];
            ll->line = p->top_line + i;
            ll->offset = l.offset;
            ll->len = l.len;
            ll->nglyphs = ng;
            ll->y = y;
            ll->glyphs = g;
        }

        /* Search matches behind selection. */
        if (p->search_query && p->search_query[0] && len > 0u)
        {
            SearchRange matches[64];
            uint32_t nm = quill_find_all(d, p->search_query, l.offset, l.offset + len, matches, 64u), mi;
            float match[4];
            quill_color_linear(0xF9E2AF55u, match);
            match[3] *= op;
            for (mi = 0u; mi < nm; ++mi)
            {
                uint32_t a = matches[mi].start - l.offset;
                uint32_t b = matches[mi].end - l.offset;
                float x1 = text_x + font_line_x_for_byte(g, ng, a) - p->scroll_x;
                float x2 = text_x + font_line_x_for_byte(g, ng, b) - p->scroll_x;
                if (x1 < text_x) x1 = text_x;
                emit_solid(dl, x1, y, x2 - x1, lh, match);
            }
        }

        /* Selections behind text. */
        {
            uint32_t si;
            for (si = 0u; si < ss->count; ++si)
            {
                uint32_t sel_a = ss_min(&ss->c[si]);
                uint32_t sel_b = ss_max(&ss->c[si]);
                if (p->modal && p->modal->mode == QMODE_VISUAL && sel_b < d->q.text_len) sel_b++;
                uint32_t a, b;
                float x1, x2;
                if (sel_a == sel_b || l.offset >= sel_b || l.offset + l.len <= sel_a) continue;
                a = sel_a > l.offset ? sel_a - l.offset : 0u;
                b = sel_b < l.offset + l.len ? sel_b - l.offset : l.len;
                if (a > len) a = len;
                if (b > len) b = len;
                x1 = text_x + font_line_x_for_byte(g, ng, a) - p->scroll_x;
                x2 = text_x + font_line_x_for_byte(g, ng, b) - p->scroll_x;
                if (x2 < x1) { float t = x1; x1 = x2; x2 = t; }
                if (x1 < text_x) x1 = text_x;
                emit_solid(dl, x1, y, x2 - x1, lh, sel);
            }
        }

        /* Line number. */
        if (p->line_numbers)
        {
            char num[32];
            const ShapedGlyph* nglyphs;
            uint32_t nn = (uint32_t)snprintf(num, sizeof num, "%u", p->top_line + i + 1u);
            uint32_t nng = font_shape_line(fs, (const uint8_t*)num, nn, p->tab_width, &nglyphs);
            float w = font_line_x_for_byte(nglyphs, nng, UINT32_MAX);
            uint8_t numcolor[32];
            memset(numcolor, QC_DEFAULT, sizeof numcolor);
            /* Gutter uses its own colour: emit manually. */
            {
                uint32_t j;
                for (j = 0u; j < nng; ++j)
                {
                    const AtlasEntry* e = font_atlas_get(fs, nglyphs[j].face,
                                                         nglyphs[j].glyph, 0u);
                    if (!e || (e->w <= 1u && e->h <= 1u)) continue;
                    emit_glyph(dl, gutter - pad - w + nglyphs[j].x_offset,
                               baseline_y - (float)e->top - nglyphs[j].y_offset,
                               e, gut_fg, aw, ah);
                }
            }
        }

        emit_shaped(dl, fs, g, ng, text_x, baseline_y, scratch->color, len,
                    lin, text_x, (float)p->width, p->scroll_x);


    }
    *nlayouts = nl;

    /* Cursors: Neovide-style critically damped springs + fading trail. */
    {
        uint32_t si;
        float blink = p->motion ? p->blink_alpha : 1.0f;
        for (si = 0u; si < ss->count; ++si)
        {
            mu_quill_cursor sc = mu_quill_offset_to_cursor(&d->q, ss->c[si].head);
            uint32_t li;
            float tx = 0.0f, ty = 0.0f, tw = 2.0f, th = lh;
            bool insert = !p->modal || p->modal->mode == QMODE_INSERT;
            bool found = false;
            for (li = 0u; li < nl; ++li)
            {
                if (layouts[li].line == sc.line)
                {
                    tx = text_x + font_line_x_for_byte(layouts[li].glyphs, layouts[li].nglyphs, sc.col) - p->scroll_x;
                    ty = layouts[li].y;
                    found = true;
                    break;
                }
            }
            if (!insert) tw = adv;
            if (!found || tx < text_x - adv || tx > (float)p->width)
            {
                if (p->cursor_anim && si < QUILL_MAX_CURSORS)
                    p->cursor_anim[si].valid = false;
                continue;
            }
            if (!p->cursor_anim || !p->motion || si >= QUILL_MAX_CURSORS)
            {
                float c[4] = { cur[0], cur[1], cur[2], cur[3] * (insert ? 1.0f : 0.45f) * blink };
                emit_solid(dl, tx, ty, tw, th, c);
                continue;
            }
            {
                CursorAnim* ca = &p->cursor_anim[si];
                const MotionConfig* cfg = p->motion;
                float len_ms = (float)cfg->cursor_ms;
                float dt = ca->valid ? (float)(p->now_ms - ca->last_ms) * 0.001f : 0.0f;
                float base_alpha = insert ? 1.0f : 0.45f;
                bool active = false;
                uint32_t ti;
                if (cfg->reduced_motion || cfg->style == MOTION_INSTANT) len_ms = 0.0f;
                if (insert && !cfg->animate_insert) len_ms = 0.0f;
                if (dt < 0.0f) dt = 0.0f;
                if (!ca->valid || len_ms <= 0.0f)
                {
                    ca->x = tx; ca->y = ty; ca->w = tw; ca->h = th;
                    ca->vx = ca->vy = ca->vw = ca->vh = 0.0f;
                    ca->valid = true;
                    ca->trail_count = 0u;
                    ca->trail_head = 0u;
                }
                else
                {
                    quill_spring_step(&ca->x, &ca->vx, tx, dt, len_ms);
                    quill_spring_step(&ca->y, &ca->vy, ty, dt, len_ms);
                    quill_spring_step(&ca->w, &ca->vw, tw, dt, len_ms);
                    quill_spring_step(&ca->h, &ca->vh, th, dt, len_ms);
                    if (quill_fabsf(ca->x - tx) < 0.05f && quill_fabsf(ca->vx) < 0.5f &&
                        quill_fabsf(ca->y - ty) < 0.05f && quill_fabsf(ca->vy) < 0.5f &&
                        quill_fabsf(ca->w - tw) < 0.05f && quill_fabsf(ca->h - th) < 0.05f)
                    {
                        ca->x = tx; ca->y = ty; ca->w = tw; ca->h = th;
                        ca->vx = ca->vy = ca->vw = ca->vh = 0.0f;
                    }
                    else
                        active = true;
                }
                ca->last_ms = p->now_ms;

                if (p->vfx && si == ss->primary)
                {
                    bool vfx_on = vfx_update(p->vfx, cfg, dt, tx, ty, tw, th,
                                             len_ms <= 0.0f, cur);
                    if (vfx_on && p->animating) *p->animating = true;
                }

                if (active && len_ms > 0.0f && cfg->trail_size > 0.0f)
                {
                    ca->trail_x[ca->trail_head] = ca->x;
                    ca->trail_y[ca->trail_head] = ca->y;
                    ca->trail_w[ca->trail_head] = ca->w;
                    ca->trail_h[ca->trail_head] = ca->h;
                    ca->trail_t[ca->trail_head] = p->now_ms;
                    ca->trail_head = (ca->trail_head + 1u) % QUILL_CURSOR_TRAIL;
                    if (ca->trail_count < QUILL_CURSOR_TRAIL) ca->trail_count++;
                }
                if (cfg->trail_size > 0.0f && len_ms > 0.0f)
                {
                    float trail_ms = len_ms * cfg->trail_size;
                    for (ti = 0u; ti < ca->trail_count; ++ti)
                    {
                        float age = (float)(p->now_ms - ca->trail_t[ti]);
                        float fade = 1.0f - age / trail_ms;
                        float c[4];
                        if (age < 0.0f || fade <= 0.0f) continue;
                        active = true;
                        c[0] = cur[0]; c[1] = cur[1]; c[2] = cur[2];
                        c[3] = cur[3] * base_alpha * 0.35f * cfg->trail_size * fade;
                        emit_solid(dl, ca->trail_x[ti], ca->trail_y[ti],
                                   ca->trail_w[ti], ca->trail_h[ti], c);
                    }
                }
                {
                    float c[4] = { cur[0], cur[1], cur[2], cur[3] * base_alpha * blink };
                    emit_solid(dl, ca->x, ca->y, ca->w, ca->h, c);
                }
                if (active && p->animating) *p->animating = true;
            }
        }
    }

    if (p->vfx && p->motion)
        vfx_render(p->vfx, p->motion, dl, adv, lh);

    /* Status bar. */
    {
        float sy = (float)p->height - status_h;
        const ShapedGlyph* sg;
        uint32_t sn, j;
        int len;
        emit_solid(dl, 0.0f, sy, (float)p->width, status_h, st_bg);
        len = snprintf(scratch->status, sizeof scratch->status, "%s  %s%s  Ln %u, Col %u  %s  %u cursor%s",
                       p->modal ? modal_mode_name(p->modal) : "NORMAL",
                       d->path[0] ? d->path : "[No Name]", d->dirty ? " *" : "",
                       cursor_line + 1u, cursor_col + 1u, syntax_name(d->syntax),
                       ss->count, ss->count == 1u ? "" : "s");
        if (len < 0) len = 0;
        if ((uint32_t)len > sizeof scratch->status - 1u)
            len = (int)sizeof scratch->status - 1u;
        sn = font_shape_line(fs, (const uint8_t*)scratch->status, (uint32_t)len,
                             p->tab_width, &sg);
        for (j = 0u; j < sn; ++j)
        {
            const AtlasEntry* e = font_atlas_get(fs, sg[j].face, sg[j].glyph, 0u);
            if (!e || (e->w <= 1u && e->h <= 1u)) continue;
            emit_glyph(dl, pad + sg[j].x_offset,
                       sy + (status_h - lh) * 0.5f + base - (float)e->top - sg[j].y_offset,
                       e, st_fg, aw, ah);
        }
    }
}

uint32_t frame_hit_test(const FrameParams* p, const LineLayout* layouts,
                        uint32_t nlayouts, float line_height, float gutter,
                        float x, float y, Document* d)
{
    uint32_t idx, i;
    float lx;
    const LineLayout* ll;
    if (nlayouts == 0u) return d->q.text_len;
    if (y < 0.0f) y = 0.0f;
    idx = (uint32_t)(y / line_height);
    if (idx >= nlayouts) idx = nlayouts - 1u;
    ll = &layouts[idx];
    lx = x - gutter - 8.0f + p->scroll_x;
    if (lx <= 0.0f) return ll->offset;
    for (i = 0u; i < ll->nglyphs; ++i)
    {
        const ShapedGlyph* g = &ll->glyphs[i];
        if (lx < g->x_offset + g->x_advance * 0.5f)
            return ll->offset + g->cluster;
    }
    return ll->offset + ll->len;
}

void frame_rect(DrawList* dl, float x, float y, float w, float h, const float c[4])
{
    emit_solid(dl, x, y, w, h, c);
}

void frame_text(FontSystem* fs, DrawList* dl, const char* text, uint32_t len,
                float x, float baseline_y, const float c[4], float clip_x1)
{
    const ShapedGlyph* g;
    uint32_t n, i;
    uint32_t aw = font_atlas_width(fs), ah = font_atlas_height(fs);
    n = font_shape_line(fs, (const uint8_t*)text, len, 4u, &g);
    for (i = 0u; i < n; ++i)
    {
        const AtlasEntry* e = font_atlas_get(fs, g[i].face, g[i].glyph, 0u);
        float gx = x + g[i].x_offset;
        if (!e || (e->w <= 1u && e->h <= 1u)) continue;
        if (gx > clip_x1) break;
        emit_glyph(dl, gx, baseline_y - (float)e->top - g[i].y_offset, e, c, aw, ah);
    }
}
