#include "terminal.h"
#include "font.h"
#include "frame.h"
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

/* Drive a real PTY whose child is a deterministic script: 30 numbered lines,
 * a green banner, then sleep. Verifies output parsing, SGR color, input echo,
 * scrollback, resize, and rendering. */

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static bool row_is(Terminal* t, uint32_t row, const char* s)
{
    uint32_t c;
    for (c = 0u; s[c]; ++c)
        if (terminal_cell_cp(t, row, c) != (uint8_t)s[c]) return false;
    return true;
}

static bool view_row_is(Terminal* t, uint32_t row, const char* s)
{
    uint32_t c;
    for (c = 0u; s[c]; ++c)
        if (terminal_view_cp(t, row, c) != (uint8_t)s[c]) return false;
    return true;
}

static bool wait_until(Terminal* t, bool (*pred)(Terminal*), int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms)
    {
        terminal_poll(t);
        if (pred(t)) return true;
        msleep(10);
        waited += 10;
    }
    terminal_poll(t);
    return pred(t);
}

static bool got_green(Terminal* t) { return row_is(t, 4u, "GREEN"); }
static bool got_echo(Terminal* t)  { return row_is(t, 4u, "GREENhi"); }

int main(void)
{
    Terminal*   t;
    FontSystem* fs;
    DrawList*   dl;
    FILE*       f;
    const char* SCRIPT = "build/term_test_shell.sh";

    mkdir("build", 0755);
    f = fopen(SCRIPT, "w");
    assert(f);
    fputs("#!/bin/sh\n"
          "i=1; while [ $i -le 30 ]; do printf 'line%d\\r\\n' $i; i=$((i+1)); done\n"
          "printf '\\033[32mGREEN\\033[0m'\n"
          "sleep 30\n", f);
    fclose(f);
    chmod(SCRIPT, 0755);
    setenv("SHELL", SCRIPT, 1);

    t = terminal_create(20u, 5u);
    assert(t);
    assert(terminal_cols(t) == 20u && terminal_rows(t) == 5u);

    /* Output parsing: wait for the whole banner, then check text + color. */
    assert(wait_until(t, got_green, 5000));
    assert(row_is(t, 0u, "line27"));
    assert(terminal_cell_cp(t, 4u, 0u) == 'G');
    assert(terminal_cell_fg(t, 4u, 0u) == 0x00CD00u); /* SGR 32 -> palette green */

    /* Input: printable chars are echoed by the PTY line discipline. */
    terminal_send_char(t, 'h');
    terminal_send_char(t, 'i');
    assert(wait_until(t, got_echo, 3000));

    /* Scrollback: 26 lines scrolled off; scrolling up reveals them. */
    terminal_scroll(t, 5);
    assert(terminal_scrolled(t));
    assert(view_row_is(t, 0u, "line22"));
    terminal_scroll(t, -100);
    assert(!terminal_scrolled(t));
    assert(view_row_is(t, 4u, "GREENhi"));

    /* Render smoke: the grid produces glyphs and a background solid. */
    fs = font_create(16u, 1u);
    dl = (DrawList*)MU_CALLOC(1u, sizeof *dl);
    assert(fs && dl);
    terminal_render(t, fs, dl, 0.0f, 0.0f, 200.0f, 100.0f);
    assert(dl->nglyphs > 0u);
    assert(dl->nsolids > 0u);
    MU_FREE(dl);
    font_destroy(fs);

    /* Resize updates the grid and the child's window size. */
    terminal_resize(t, 10u, 3u);
    assert(terminal_cols(t) == 10u && terminal_rows(t) == 3u);

    terminal_destroy(t);
    unlink(SCRIPT);
    puts("test_terminal: PASS");
    return 0;
}
