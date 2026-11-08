/* Probe: does the picker actually close when the user selects an item?
 * Drives the real app callbacks (app_on_key / app_on_char / picker_hit_test)
 * the same way GLFW would. */
#define main quill_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <unistd.h>

static App A;

static void reset(void)
{
    if (A.picker) { picker_destroy(A.picker); MU_FREE(A.picker); A.picker = NULL; }
    A.search_open = false;
}

int main(void)
{
    memset(&A, 0, sizeof A);
    A.window = NULL;
    config_defaults(&A.cfg);
    A.cfg.session_restore = false;
    A.params.width = 1280;
    A.params.height = 800;
    A.fs = font_create(16u, 1u);
    assert(A.fs);
    assert(app_init_doc(&A, NULL));

    /* A. keyboard select (Enter) on the command palette */
    reset();
    open_picker_source(&A, PSRC_COMMANDS);
    assert(A.picker && A.picker->open && A.picker->count > 0u);
    app_on_key(&A, GLFW_KEY_ENTER, 0, GLFW_PRESS, 0);
    printf("A: enter on palette -> open=%d accepted=%d\n", (int)A.picker->open, (int)A.picker->accepted);

    /* B. keyboard select on the file picker (async job results) */
    reset();
    open_picker_source(&A, PSRC_FILES);
    for (int i = 0; i < 200 && A.picker->count == 0u; ++i) { picker_poll(A.picker); usleep(5000); }
    printf("B: file results=%u\n", A.picker->count);
    app_on_key(&A, GLFW_KEY_ENTER, 0, GLFW_PRESS, 0);
    printf("B: enter on files -> open=%d accepted=%d\n", (int)A.picker->open, (int)A.picker->accepted);

    /* C. mouse click select on the command palette */
    reset();
    open_picker_source(&A, PSRC_COMMANDS);
    assert(A.picker && A.picker->open && A.picker->count > 0u);
    {
        float lh = font_line_height(A.fs);
        float w = (float)A.params.width * 0.86f, h = (float)A.params.height * 0.82f;
        if (w > 1100.0f) w = 1100.0f;
        if (h > 720.0f) h = 720.0f;
        float px = ((float)A.params.width - w) * 0.5f;
        float py = ((float)A.params.height - h) * 0.5f;
        float pad = 10.0f, rowh = lh + 4.0f;
        float cx = px + 20.0f;
        float cy = py + pad + lh + 10.0f + 1.5f * rowh; /* middle of row 1 */
        int hit = picker_hit_test(A.picker, A.params.width, A.params.height, lh, cx, cy);
        printf("C: center of row 1 -> hit=%d (lh=%.1f)\n", hit, lh);
        if (hit >= 0)
        {
            A.picker->selected = (uint32_t)hit;
            A.picker->result = A.picker->items[hit];
            A.picker->accepted = true;
            A.picker->open = false;
        }
        printf("C: after click -> open=%d\n", (int)A.picker->open);
    }

    /* D. does anything re-open the picker right after accepting? */
    reset();
    open_picker_source(&A, PSRC_COMMANDS);
    {
        PickerItem* it;
        A.picker->selected = 0u;
        A.picker->result = A.picker->items[0];
        A.picker->accepted = true;
        A.picker->open = false;
        it = &A.picker->result;
        (void)it;
        accept_picker(&A);
        printf("D: after accept_picker(%s) -> open=%d\n", A.picker->result.label, (int)A.picker->open);
    }

    reset();
    printf("done\n");
    return 0;
}
