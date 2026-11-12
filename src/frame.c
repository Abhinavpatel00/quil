#include "frame.h"
#include "vfx.h"
#include "search.h"
#include "theme.h"

/* QUILL_THEME (the active palette) is defined in theme.c and can be replaced
 * at runtime by a theme file; the built-in default is Noir. */

static void emit_solid_r(DrawList* dl, float x, float y, float w, float h,
                         const float c[4], float radius)
{
    SolidQuad* q;
    uint32_t*  n;
    SolidQuad* arr;
    if (w <= 0.0f || h <= 0.0f) return;
    if (dl->in_overlay)
    {
        n = &dl->nov_solids;
        arr = dl->ov_solids;
        if (*n >= QUILL_MAX_OVERLAY_SOLIDS) { dl->overflow++; return; }
    }
    else
    {
        n = &dl->nsolids;
        arr = dl->solids;
        if (*n >= QUILL_MAX_SOLIDS) { dl->overflow++; return; }
    }
    q = &arr[(*n)++];
    q->x = x; q->y = y; q->w = w; q->h = h;
    q->r = c[0]; q->g = c[1]; q->b = c[2]; q->a = c[3];
    q->radius = radius > 0.0f ? radius : 0.0f;
}

static void emit_solid(DrawList* dl, float x, float y, float w, float h,
                       const float c[4])
{
    emit_solid_r(dl, x, y, w, h, c, 0.0f);
}

static void emit_glyph(DrawList* dl, float x, float y, const AtlasEntry* e,
                       const float c[4], FontSystem* fs)
{
    GlyphQuad* q;
    uint32_t*  n;
    GlyphQuad* arr;
    uint32_t atlas_w, atlas_h;
    if (!e || e->w == 0u || e->h == 0u) return;
    /* Read the atlas size *now*: font_atlas_get() can grow the atlas while a
     * frame is being built, and UVs computed with the previous size sample the
     * wrong region. */
    atlas_w = font_atlas_width(fs);
    atlas_h = font_atlas_height(fs);
    if (atlas_w == 0u || atlas_h == 0u) return;
    if (dl->in_overlay)
    {
        n = &dl->nov_glyphs;
        arr = dl->ov_glyphs;
        if (*n >= QUILL_MAX_OVERLAY_GLYPHS) { dl->overflow++; return; }
    }
    else
    {
        n = &dl->nglyphs;
        arr = dl->glyphs;
        if (*n >= QUILL_MAX_GLYPHS) { dl->overflow++; return; }
    }
    q = &arr[(*n)++];
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
        emit_glyph(dl, gx, gy, e, lin[ci], fs);
    }
}

/* Pick whichever of two theme colours reads better on `bg`. Palettes are user
 * supplied, so `accent` can legitimately sit close to `status_bg`; writing the
 * mode label in status_bg unconditionally would make it vanish there. */
static void text_on(const float bg[4], const float a[4], const float b[4], float out[4])
{
    float lb = 0.2126f * bg[0] + 0.7152f * bg[1] + 0.0722f * bg[2];
    float la = 0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2];
    float lc = 0.2126f * b[0] + 0.7152f * b[1] + 0.0722f * b[2];
    const float* pick = quill_fabsf(la - lb) >= quill_fabsf(lc - lb) ? a : b;
    out[0] = pick[0]; out[1] = pick[1]; out[2] = pick[2]; out[3] = pick[3];
}

void frame_build(Document* d, FontSystem* fs, const FrameParams* p,
                 DrawList* dl, FrameScratch* scratch,
                 LineLayout* layouts, uint32_t* nlayouts)
{
    float lin[16][4];
    float bg[4], fg[4], gut_bg[4], gut_fg[4], line_hl[4], sel[4], cur[4];
    float st_bg[4], st_fg[4], accent[4], guide[4];
    bool guides;
    float lh, base, adv, pad, gutter, text_x, status_h, text_h, op;
    uint32_t rows, i, digits, nlines, cursor_line, cursor_col;
    uint32_t nsp = 0u;
    mu_quill_view view;
    mu_quill_cursor cc;
    uint32_t nl = 0u;
    SelectionSet fallback;
    const SelectionSet* ss = p->selections;
    const SelCursor* prim;

    dl->nsolids = 0u;
    dl->nglyphs = 0u;
    dl->nvfx = 0u;
    /* The overlay layer is only filled while something is open, so clearing it
     * there is not enough: on the first frame after the picker closes nothing
     * calls frame_overlay_begin(), the previous frame's ov_solids/ov_glyphs stay
     * live, and the renderer keeps drawing that stale overlay. */
    dl->nov_solids = 0u;
    dl->nov_glyphs = 0u;
    dl->in_overlay = false;
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
    quill_color_linear(QUILL_THEME.accent, accent);
    /* Indent guides are a hint, not a feature. The colour is the foreground
     * scaled down in linear space, so the on-screen result is roughly a 15%
     * grey on a dark theme: visible against the wallpaper, never competing
     * with text. */
    guide[0] = fg[0]; guide[1] = fg[1]; guide[2] = fg[2]; guide[3] = fg[3] * 0.045f;
    guides = p->motion && p->motion->indent_guides;

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
        accent[3] *= op;
        guide[3] *= op;
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

        /* Indent guides: one faint rule per indent stop, behind everything
         * else on the line. Columns are expanded the same way the shaper does
         * it (tabs advance to the next tab stop), so the rules line up with
         * both space- and tab-indented code. */
        if (guides && len > 0u)
        {
            uint32_t tw = p->tab_width ? p->tab_width : 1u;
            uint32_t k2 = 0u, col = 0u, gc;
            while (k2 < len)
            {
                unsigned char ch = scratch->line[k2];
                if (ch == ' ') col++;
                else if (ch == '\t')
                    col = (col / tw + 1u) * tw;
                else break;
                k2++;
            }
            /* Draw at every stop up to and including the line's own indent:
             * a 4-space block gets one rule under its first character, a
             * nested 8-space block gets two. Stops strictly below the indent
             * (the first version) meant ordinary 4-space code showed none. */
            for (gc = tw; gc <= col; gc += tw)
            {
                float gx = quill_floorf(text_x + (float)gc * adv - p->scroll_x);
                if (gx >= text_x && gx + 1.0f <= (float)p->width)
                    emit_solid(dl, gx, y, 1.0f, lh, guide);
            }
        }

        /* Search matches behind selection. */
        if (p->search_query && p->search_query[0] && len > 0u)
        {
            SearchRange matches[64];
            uint32_t nm = quill_find_all(d, p->search_query, l.offset, l.offset + len, matches, 64u), mi;
            float match[4];
            quill_color_linear(QUILL_THEME.search, match);
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
                               e, gut_fg, fs);
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
            /* Snap the animation target to whole device pixels.
             *
             * Glyphs are emitted at quill_floorf(x) (see the emit path above),
             * but the cursor rectangle was animated toward the *unrounded*
             * advance, which is fractional for any font whose advance is not an
             * integer (Fira Code at 16px is 9.6px). The spring was therefore
             * chasing a position it could never settle on, and the 0.05px
             * settle test below never fired: every cursor move kept the app in
             * its 250Hz animating loop for ~0.7s. Snapping here makes the
             * cursor crisp (it no longer lands between pixels) and gives the
             * spring a reachable target. */
            tx = quill_floorf(tx);
            ty = quill_floorf(ty);
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
                /* Use the caller's high-resolution delta so cursor and VFX
                 * motion do not step in whole milliseconds; fall back to the
                 * millisecond clock for harnesses that only set now_ms. */
                float dt = 0.0f;
                if (ca->valid)
                {
                    dt = p->dt > 0.0f ? p->dt
                         : (p->now_ms > ca->last_ms ? (float)(p->now_ms - ca->last_ms) * 0.001f : 0.0f);
                }
                if (dt < 0.0f) dt = 0.0f;
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
                    /* Settle when the cursor is within half a pixel and
                     * moving slowly. The old 0.05px / 0.5px-per-frame pair was
                     * stricter than the spring's own tail, so "settled" was
                     * reached tens of frames after the eye could no longer see
                     * motion, and each of those frames cost a full render. */
                    if (quill_fabsf(ca->x - tx) < 0.5f && quill_fabsf(ca->vx) < 5.0f &&
                        quill_fabsf(ca->y - ty) < 0.5f && quill_fabsf(ca->vy) < 5.0f &&
                        quill_fabsf(ca->w - tw) < 0.5f && quill_fabsf(ca->h - th) < 0.5f)
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
                    /* A real cursor move is a change of logical target.  The
                     * pixel target also moves every frame during smooth
                     * scrolling, and treating that as a move restarts the
                     * highlight rings continuously, which looks like a pulse
                     * that never expands. */
                    bool target_moved = !ca->target_valid ||
                                        ca->target_line != sc.line || ca->target_col != sc.col;
                    ca->target_line = sc.line;
                    ca->target_col = sc.col;
                    ca->target_valid = true;
                    vfx_notify_cursor(p->vfx, target_moved);
                    /* Emit from the cursor position that is actually on screen
                     * this frame, not from the spring's destination.  Using
                     * the destination made a jump dump its whole particle
                     * burst on one frame while the cursor was still sliding
                     * toward it: the trail popped into existence ahead of the
                     * cursor and then sat there, which reads as jitter.  The
                     * animated position spreads the same number of particles
                     * evenly over the movement, so the trail is drawn by the
                     * cursor as it travels. */
                    bool vfx_on = vfx_update(p->vfx, cfg, dt, ca->x, ca->y, ca->w, ca->h,
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

    /* Status bar: an accent chip carrying the current mode, then the details.
     * The chip is what makes INSERT/VISUAL obvious without reading the text,
     * and it gives the palette's accent colour a real job (it used to be used
     * only by the picker's top rule). */
    {
        float sy = (float)p->height - status_h;
        const char* mode = p->modal ? modal_mode_name(p->modal) : "NORMAL";
        uint32_t mlen = (uint32_t)strlen(mode);
        const ShapedGlyph* sg;
        uint32_t sn, j;
        float chip_w, chip_fg[4];
        int len;
        emit_solid(dl, 0.0f, sy, (float)p->width, status_h, st_bg);
        text_on(accent, st_bg, st_fg, chip_fg);
        sn = font_shape_line(fs, (const uint8_t*)mode, mlen, p->tab_width, &sg);
        chip_w = font_line_x_for_byte(sg, sn, UINT32_MAX) + 2.0f * pad;
        emit_solid(dl, 0.0f, sy, chip_w, status_h, accent);
        for (j = 0u; j < sn; ++j)
        {
            const AtlasEntry* e = font_atlas_get(fs, sg[j].face, sg[j].glyph, 0u);
            if (!e || (e->w <= 1u && e->h <= 1u)) continue;
            emit_glyph(dl, pad + sg[j].x_offset,
                       sy + (status_h - lh) * 0.5f + base - (float)e->top - sg[j].y_offset,
                       e, chip_fg, fs);
        }
        len = snprintf(scratch->status, sizeof scratch->status, "  %s%s  Ln %u, Col %u  %s  %u cursor%s",
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
            emit_glyph(dl, chip_w + pad + sg[j].x_offset,
                       sy + (status_h - lh) * 0.5f + base - (float)e->top - sg[j].y_offset,
                       e, st_fg, fs);
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

void frame_overlay_begin(DrawList* dl)
{
    if (!dl || dl->in_overlay) return;
    dl->nov_solids = 0u;
    dl->nov_glyphs = 0u;
    dl->in_overlay = true;
}

void frame_overlay_end(DrawList* dl)
{
    if (!dl) return;
    dl->in_overlay = false;
}

void frame_rect(DrawList* dl, float x, float y, float w, float h, const float c[4])
{
    emit_solid(dl, x, y, w, h, c);
}

void frame_rect_rounded(DrawList* dl, float x, float y, float w, float h,
                        const float c[4], float radius)
{
    emit_solid_r(dl, x, y, w, h, c, radius);
}

void frame_text(FontSystem* fs, DrawList* dl, const char* text, uint32_t len,
                float x, float baseline_y, const float c[4], float clip_x1)
{
    const ShapedGlyph* g;
    uint32_t n, i;
    n = font_shape_line(fs, (const uint8_t*)text, len, 4u, &g);
    for (i = 0u; i < n; ++i)
    {
        const AtlasEntry* e = font_atlas_get(fs, g[i].face, g[i].glyph, 0u);
        float gx = x + g[i].x_offset;
        if (!e || (e->w <= 1u && e->h <= 1u)) continue;
        if (gx > clip_x1) break;
        emit_glyph(dl, gx, baseline_y - (float)e->top - g[i].y_offset, e, c, fs);
    }
}
