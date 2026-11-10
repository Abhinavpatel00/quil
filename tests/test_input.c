/* Integration test for the real GLFW input dispatch (app_on_key/app_on_char).
 * Includes main.c so it can drive the same code path the window callbacks use,
 * without needing a live GLFW window. Proves commands follow the layout char,
 * not the QWERTY physical key. */
#define main quill_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <unistd.h>

static App A;

static void reset(void)
{
    if (A.picker) { picker_destroy(A.picker); MU_FREE(A.picker); A.picker = NULL; }
    modal_init(&A.modal);
    ss_single(&A.ss, 0u);
    ss_sync_doc(&A.ss, A.doc);
    A.search_open = false;
}

static bool palette_open(void)
{
    return A.picker && A.picker->open && A.picker->source == PSRC_COMMANDS;
}

static float shaped_width(const char* text, uint32_t len)
{
    const ShapedGlyph* glyphs = NULL;
    uint32_t n = font_shape_line(A.fs, (const uint8_t*)text, len,
                                 A.params.tab_width, &glyphs);
    return font_line_x_for_byte(glyphs, n, UINT32_MAX);
}

/* One frame of scroll animation: params.scroll_x is the *rendered* offset, so
 * it only reflects a policy change once the view has been advanced. With the
 * dramatic camera off this is a plain copy from the target. */
static void advance_a_frame(void)
{
    bool active = false;
    advance_view(&A, 1.0f / 60.0f, &active);
}

/* Simulate GLFW delivering a ':' typed on an arbitrary physical key. */
static void type_colon(int physical_key, int mods)
{
    app_on_key(&A, physical_key, 0, GLFW_PRESS, mods);
    app_on_char(&A, ':');
}

int main(void)
{
    memset(&A, 0, sizeof A);
    A.window = NULL;
    config_defaults(&A.cfg);
    A.cfg.session_restore = false;   /* deterministic: no saved session */
    A.params.width = 1280;
    A.params.height = 800;
    A.fs = font_create(16u, 1u);
    assert(A.fs);
    assert(app_init_doc(&A, NULL));   /* sample C buffer, no window needed */

    /* 1. QWERTY position: Shift+; produces ':'. */
    reset();
    type_colon(GLFW_KEY_SEMICOLON, GLFW_MOD_SHIFT);
    assert(palette_open());

    /* 2. Non-QWERTY: ':' lives on a letter key with NO shift. The US mapping
     *    of that physical key is 'z', not ':' — the command must still fire
     *    because the layout char is ':'. This is the regression case. */
    reset();
    type_colon(GLFW_KEY_Z, 0);
    assert(palette_open());

    /* 3. Layouts that emit GLFW_KEY_UNKNOWN for keys with no US counterpart. */
    reset();
    type_colon(GLFW_KEY_UNKNOWN, 0);
    assert(palette_open());

    /* 4. AltGr-style ':' (Ctrl+Alt on some layouts) must not be swallowed. */
    reset();
    type_colon(GLFW_KEY_Q, GLFW_MOD_CONTROL | GLFW_MOD_ALT);
    assert(palette_open());

    /* 5. In INSERT mode the same ':' is literal text, not a command. */
    reset();
    app_on_char(&A, 'i');                 /* enter insert via layout char */
    assert(A.modal.mode == QMODE_INSERT);
    {
        uint32_t before = A.doc->q.text_len;
        app_on_key(&A, GLFW_KEY_Z, 0, GLFW_PRESS, 0);
        app_on_char(&A, ':');
        assert(!palette_open());
        assert(A.doc->q.text_len == before + 1u);
    }

    /* 6. Non-char command keys are NOT deferred: arrows still move cursor. */
    reset();
    ss_single(&A.ss, 0u); ss_sync_doc(&A.ss, A.doc);
    app_on_key(&A, GLFW_KEY_DOWN, 0, GLFW_PRESS, 0);
    assert(mu_quill_offset_to_cursor(&A.doc->q, A.ss.c[0].head).line == 1u);

    /* 7. Physical Ctrl shortcut is handled by the key path, not deferred to the
     *    char callback. Clear the path so Ctrl-S is a safe no-op save. */
    reset();
    A.doc->path[0] = '\0';
    A.doc->dirty = true;
    app_on_key(&A, GLFW_KEY_S, 0, GLFW_PRESS, GLFW_MOD_CONTROL);
    assert(!palette_open());

    /* 8. Ctrl-: toggles in both directions without leaking its ':' char.
     *    Ctrl+Alt is AltGr: route its layout char to the terminal instead of
     *    encoding the physical key. Bépo AltGr+X -> '{' is the regression. */
    reset();
    assert(!A.term_active);
    {
        const char* script = "build/input_test_shell.sh";
        FILE* f = fopen(script, "w");
        uint32_t i;
        assert(f);
        fputs("#!/bin/sh\n"
              "stty -echo\n"
              "cat\n", f);
        fclose(f);
        assert(chmod(script, 0755) == 0);
        setenv("SHELL", script, 1);

        app_on_key(&A, GLFW_KEY_SEMICOLON, 0, GLFW_PRESS,
                   GLFW_MOD_CONTROL | GLFW_MOD_SHIFT);
        app_on_char(&A, ':'); /* must not become shell input */
        assert(A.term && A.term_active);
        for (i = 0u; i < 50u; ++i) { terminal_poll(A.term); usleep(10000); }

        app_on_key(&A, GLFW_KEY_X, 0, GLFW_PRESS,
                   GLFW_MOD_CONTROL | GLFW_MOD_ALT);
        app_on_char(&A, '{');
        app_on_key(&A, GLFW_KEY_X, 0, GLFW_PRESS, 0);
        app_on_char(&A, 'x');
        app_on_key(&A, GLFW_KEY_ENTER, 0, GLFW_PRESS, 0);
        for (i = 0u; i < 300u; ++i)
        {
            terminal_poll(A.term);
            if (terminal_cell_cp(A.term, 0u, 0u) == '{' &&
                terminal_cell_cp(A.term, 0u, 1u) == 'x') break;
            usleep(10000);
        }
        assert(terminal_cell_cp(A.term, 0u, 0u) == '{');
        assert(terminal_cell_cp(A.term, 0u, 1u) == 'x');
        assert(terminal_cell_cp(A.term, 0u, 2u) == 0u);

        app_on_key(&A, GLFW_KEY_SEMICOLON, 0, GLFW_PRESS,
                   GLFW_MOD_CONTROL | GLFW_MOD_SHIFT);
        app_on_char(&A, ':'); /* must not open the command palette */
        assert(A.term && !A.term_active);
        assert(!palette_open());
        term_close(&A);
        assert(!A.term);
        unlink(script);
    }

    /* 9. Cursor visibility includes horizontal scrolling. Moving left from a
     *    long, scrolled line must reveal the previous offscreen character;
     *    shaped-line measurement accounts for tab expansion. */
    reset();
    A.params.width = 100u;
    A.params.line_numbers = false;
    A.params.tab_width = QUILL_TAB_WIDTH;
    {
        char long_line[161];
        memset(long_line, 'x', 160u);
        long_line[160] = '\n';
        doc_set_text(A.doc, long_line, sizeof long_line);
        ss_single(&A.ss, 80u);
        ss_sync_doc(&A.ss, A.doc);
        A.target_x = A.view_x = A.params.scroll_x = 0.0f;
        ensure_cursor_visible(&A);
        advance_a_frame();
        assert(A.params.scroll_x > 0.0f); /* right edge follows the cursor */

        ss_single(&A.ss, 5u);
        ss_sync_doc(&A.ss, A.doc);
        app_on_key(&A, GLFW_KEY_LEFT, 0, GLFW_PRESS, 0);
        assert(A.ss.c[0].head == 4u);
        advance_a_frame();
        assert(quill_fabsf(A.params.scroll_x - shaped_width("xxxx", 4u)) < 0.001f);
        ss_single(&A.ss, 0u);
        ss_sync_doc(&A.ss, A.doc);
        ensure_cursor_visible(&A);
        advance_a_frame();
        assert(A.params.scroll_x == 0.0f);

        doc_set_text(A.doc, "\t123456789\n", 11u);
        ss_single(&A.ss, 1u); /* immediately after the tab */
        ss_sync_doc(&A.ss, A.doc);
        A.target_x = A.view_x = A.params.scroll_x = 100.0f;
        ensure_cursor_visible(&A);
        advance_a_frame();
        assert(quill_fabsf(A.params.scroll_x - shaped_width("\t", 1u)) < 0.001f);
    }

    /* 10. A directory argument starts on an empty buffer and opens the files
     *     picker rooted at that folder. */
    bm_destroy(&A.bm);
    (void)mkdir("build/test_project_dir", 0755);
    {
        FILE* f = fopen("build/test_project_dir/only.c", "w");
        assert(f);
        fputs("int x;\n", f);
        fclose(f);
    }
    assert(app_init_doc(&A, "build/test_project_dir"));
    assert(A.open_files_on_start);
    assert(strcmp(A.root, "build/test_project_dir") == 0);
    assert(A.doc && A.doc->path[0] == 0);
    open_picker_source(&A, PSRC_FILES);
    assert(A.picker && A.picker->open && A.picker->source == PSRC_FILES);
    assert(strcmp(A.picker->root, "build/test_project_dir") == 0);
    {
        uint32_t i;
        for (i = 0u; i < 200u && A.picker->count == 0u; ++i)
        {
            picker_poll(A.picker);
            usleep(10000);
        }
    }
    picker_poll(A.picker);
    assert(A.picker->count == 1u);
    assert(strcmp(A.picker->items[0].path, "only.c") == 0);

    if (A.picker) { picker_destroy(A.picker); MU_FREE(A.picker); A.picker = NULL; }
    bm_destroy(&A.bm);
    font_destroy(A.fs);
    puts("test_input: PASS");
    return 0;
}
