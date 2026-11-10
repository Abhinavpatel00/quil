/* Dramatic camera mode: the scroll policy that parks the cursor at a lead
 * point of the viewport instead of scrolling the minimum distance needed to
 * reveal it. Includes main.c so it exercises the same ensure_cursor_visible /
 * advance_view the real window loop uses, without a GLFW window or a Vulkan
 * device.
 *
 * The important half of this test is the *off* half: dramatic mode must be a
 * pure opt-in. When it is off the view has to behave exactly as before (view_x
 * tracking target_x with no spring, minimal horizontal scroll), which is what
 * keeps the feature at one branch per frame for anyone who never enables it. */
#define main quill_main
#include "../src/main.c"
#undef main

#include <assert.h>

static App A;

static const char* LONG_LINE =
    "int x = 1; /* a deliberately long line, so the horizontal policy has room "
    "to choose between minimal scrolling and a 1/3 lead park */ int end;\n";

static void setup(bool dramatic)
{
    uint32_t i, bytes = 0u;
    memset(&A, 0, sizeof A);
    A.doc = (Document*)MU_CALLOC(1u, sizeof *A.doc);
    assert(A.doc && doc_init(A.doc, 8u << 20));
    A.fs = font_create(16u, 1u);
    assert(A.fs);
    /* 200 lines: three short lines per long one, so vertical and horizontal
     * policy can both be checked in the same fixture. */
    for (i = 0u; i < 200u; ++i)
    {
        const char* line = (i % 4u == 0u) ? LONG_LINE : "short;\n";
        uint32_t n = (uint32_t)strlen(line);
        assert(doc_insert(A.doc, bytes, line, n, 0u));
        bytes += n;
    }
    ss_init(&A.ss);
    ss_single(&A.ss, 0u);
    ss_sync_doc(&A.ss, A.doc);
    modal_init(&A.modal);
    config_defaults(&A.cfg);
    A.cfg.dramatic = dramatic;
    A.params.width = 1000u;
    A.params.height = 700u;
    A.params.tab_width = 4u;
    A.params.line_numbers = true;
    A.params.selections = &A.ss;
    A.params.modal = &A.modal;
}

static void teardown(void)
{
    if (A.fs) font_destroy(A.fs);
    if (A.doc) { doc_destroy(A.doc); MU_FREE(A.doc); }
    memset(&A, 0, sizeof A);
}

/* Put the primary cursor on `line` at byte `col`. */
static void put_cursor(uint32_t line, uint32_t col)
{
    uint32_t off = mu_quill_line_offset(&A.doc->q, line) + col;
    ss_single(&A.ss, off);
    ss_sync_doc(&A.ss, A.doc);
}

static CursorMetrics metrics(void)
{
    CursorMetrics m;
    mu_quill_cursor c = mu_quill_offset_to_cursor(&A.doc->q,
                                                 ss_primary_const(&A.ss)->head);
    cursor_metrics(&A, c, &m);
    return m;
}

/* Minimal scrolling (dramatic off): the cursor ends flush with the right edge
 * of the text area, never parked at a lead point. */
static void test_minimal_scroll_when_off(void)
{
    CursorMetrics m;
    setup(false);
    put_cursor(0u, 110u); /* past the right edge of a 1000px window */
    m = metrics();
    assert(m.x > m.text_w); /* the cursor really is off screen */
    ensure_cursor_visible(&A);
    assert(A.target_x > 0.0f);
    /* Right edge of the cursor sits at the right edge of the text area. */
    assert(quill_fabsf((m.text_x + m.x + m.w - A.target_x) -
                       (m.text_x + m.text_w)) < 1.5f);
    teardown();
}

/* With the camera on, the same cursor is parked at lead_x of the text area. */
static void test_lead_park_when_on(void)
{
    CursorMetrics m;
    float screen_x;
    setup(true);
    put_cursor(0u, 110u);
    m = metrics();
    ensure_cursor_visible(&A);
    screen_x = m.text_x + m.x - A.target_x;
    assert(quill_fabsf(screen_x -
                       (m.text_x + A.cfg.dramatic_lead_x * m.text_w)) < 1.5f);
    teardown();
}

/* Short lines clamp at the left edge instead of drifting into empty space. */
static void test_lead_clamps_at_line_start(void)
{
    setup(true);
    put_cursor(1u, 0u); /* a "short;" line: nothing to scroll */
    ensure_cursor_visible(&A);
    assert(A.target_x == 0.0f);
    teardown();
}

/* Vertical: the cursor line lands at lead_y of the viewport rather than at the
 * top or bottom edge. */
static void test_vertical_lead(void)
{
    uint32_t rows;
    setup(true);
    rows = visible_rows(&A);
    assert(rows > 5u);
    put_cursor(120u, 0u);
    ensure_cursor_visible(&A);
    assert(quill_fabsf(A.target_top -
                       ((float)120u - (float)rows * A.cfg.dramatic_lead_y)) < 0.01f);
    teardown();

    /* Off: line 120 is revealed by scrolling the minimum amount, so the line
     * ends up at the bottom edge instead of a third of the way down. */
    setup(false);
    put_cursor(120u, 0u);
    ensure_cursor_visible(&A);
    assert(quill_fabsf(A.target_top - ((float)120u - (float)visible_rows(&A) + 1.0f)) < 0.01f);
    teardown();
}

/* The spring: one frame moves part of the way, then it settles. */
static void test_spring_converges(void)
{
    bool active = false;
    int i;
    float first;
    setup(true);
    put_cursor(0u, 110u);
    ensure_cursor_visible(&A);
    assert(A.target_x > 50.0f);
    A.view_x = 0.0f;
    A.scroll_x_vel = 0.0f;
    A.params.scroll_x = 0.0f;
    advance_view(&A, 1.0f / 60.0f, &active);
    first = A.view_x;
    assert(first > 0.0f && first < A.target_x); /* eased, not snapped */
    assert(A.params.scroll_x == A.view_x);      /* renderer sees the view */
    assert(active);
    /* advance_view only ever raises the flag, so each simulated frame needs
     * its own (the app loop re-initialises it per frame). */
    for (i = 0; i < 240; ++i)
    {
        bool frame_active = false;
        advance_view(&A, 1.0f / 60.0f, &frame_active);
        if (i == 239) assert(!frame_active); /* settled: nothing left to animate */
    }
    assert(quill_fabsf(A.view_x - A.target_x) < 0.01f);
    teardown();
}

/* Off: no spring at all, and the rendered offset equals the target on the very
 * first frame. This is the "no overhead" guarantee in behavioural form. */
static void test_off_is_instant(void)
{
    bool active = false;
    setup(false);
    put_cursor(0u, 110u);
    ensure_cursor_visible(&A);
    assert(A.target_x > 0.0f);
    A.view_x = 0.0f;
    advance_view(&A, 1.0f / 60.0f, &active);
    assert(A.view_x == A.target_x);
    assert(A.params.scroll_x == A.target_x);
    assert(A.scroll_x_vel == 0.0f);
    teardown();
}

/* Reduced motion is respected even with the camera on. */
static void test_reduced_motion_snaps(void)
{
    bool active = false;
    setup(true);
    A.cfg.reduced_motion = true;
    put_cursor(0u, 110u);
    ensure_cursor_visible(&A);
    assert(A.target_x > 50.0f);
    A.view_x = 0.0f;
    advance_view(&A, 1.0f / 60.0f, &active);
    assert(A.view_x == A.target_x);
    assert(!active);
    teardown();
}

/* Toggling at runtime leaves the view where the other policy expects it. */
static void test_toggle_off_drops_view(void)
{
    setup(true);
    put_cursor(0u, 110u);
    ensure_cursor_visible(&A);
    A.view_x = A.target_x;
    execute_command(&A, "Dramatic Mode");
    assert(!A.cfg.dramatic);
    assert(A.target_x == A.view_x);
    assert(A.scroll_x_vel == 0.0f);
    execute_command(&A, "Dramatic Mode");
    assert(A.cfg.dramatic);
    teardown();
}

/* Documented config keys parse, and lead fractions are clamped to 0..0.5. */
static void test_config_keys(void)
{
    FILE* f = fopen("build/test_dramatic.conf", "w");
    MotionConfig cfg;
    assert(f);
    fputs("dramatic = true\n"
          "dramatic_lead_x = 0.4\n"
          "dramatic_lead_y = 0.9\n"
          "dramatic_ms = 90\n", f);
    fclose(f);
    config_load_file(&cfg, "build/test_dramatic.conf");
    assert(cfg.dramatic);
    assert(quill_fabsf(cfg.dramatic_lead_x - 0.4f) < 0.001f);
    assert(quill_fabsf(cfg.dramatic_lead_y - 0.5f) < 0.001f); /* clamped */
    assert(cfg.dramatic_ms == 90u);
    remove("build/test_dramatic.conf");

    config_defaults(&cfg);
    assert(!cfg.dramatic); /* opt-in */
    assert(quill_fabsf(cfg.dramatic_lead_x - 0.30f) < 0.001f);
    assert(quill_fabsf(cfg.dramatic_lead_y - 0.33f) < 0.001f);
    assert(cfg.dramatic_ms == 180u);
}

int main(void)
{
    test_minimal_scroll_when_off();
    test_lead_park_when_on();
    test_lead_clamps_at_line_start();
    test_vertical_lead();
    test_spring_converges();
    test_off_is_instant();
    test_reduced_motion_snaps();
    test_toggle_off_drops_view();
    test_config_keys();
    puts("test_dramatic: PASS");
    return 0;
}
