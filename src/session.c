#include "session.h"
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

/* Expand a leading ~/ against $HOME. Load/save-time only. */
static void expand_home(const char* in, char* out, size_t outsz)
{
    if (in[0] == '~' && (in[1] == '/' || in[1] == 0))
    {
        const char* home = getenv("HOME");
        if (home && home[0])
        {
            snprintf(out, outsz, "%s%s", home, in + 1);
            return;
        }
    }
    snprintf(out, outsz, "%s", in);
}

static void mkdir_parents(const char* path)
{
    char tmp[1024];
    char* p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; ++p)
    {
        if (*p == '/')
        {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/* Absolute path for a buffer, falling back to the stored path. */
static void abs_path(const char* in, char* out, size_t outsz)
{
    char resolved[PATH_MAX];
    if (in[0] && realpath(in, resolved))
        snprintf(out, outsz, "%s", resolved);
    else
        snprintf(out, outsz, "%s", in);
}

bool session_save(const char* path, const BufferManager* bm)
{
    char expanded[1024];
    char current[PATH_MAX];
    FILE* f;
    uint32_t i;
    if (!path || !path[0] || !bm) return false;
    expand_home(path, expanded, sizeof expanded);
    mkdir_parents(expanded);
    f = fopen(expanded, "w");
    if (!f) return false;

    current[0] = 0;
    if (bm->count > 0u && bm->current < bm->count && bm->docs[bm->current])
        abs_path(bm->docs[bm->current]->path, current, sizeof current);

    fprintf(f, "quill-session 1\n");
    fprintf(f, "current %s\n", current);
    for (i = 0u; i < bm->count; ++i)
    {
        const Document* d = bm->docs[i];
        char full[PATH_MAX];
        if (!d || !d->path[0]) continue; /* skip unnamed buffers */
        abs_path(d->path, full, sizeof full);
        fprintf(f, "begin\n");
        fprintf(f, "path %s\n", full);
        fprintf(f, "cursor %u\n", d->cursor);
        fprintf(f, "anchor %u\n", d->anchor);
        fprintf(f, "top %.6f\n", (double)d->view_top);
        fprintf(f, "scrollx %.6f\n", (double)d->scroll_x);
        fprintf(f, "end\n");
    }
    fclose(f);
    return true;
}

uint32_t session_load(const char* path, BufferManager* bm)
{
    char expanded[1024];
    char line[1200];
    char current[PATH_MAX];
    char e_path[PATH_MAX];
    uint32_t e_cursor = 0u, e_anchor = 0u, count = 0u, i;
    float e_top = 0.0f, e_scrollx = 0.0f;
    bool in = false;
    FILE* f;
    if (!path || !path[0] || !bm) return 0u;
    expand_home(path, expanded, sizeof expanded);
    f = fopen(expanded, "r");
    if (!f) return 0u;
    if (!fgets(line, sizeof line, f) || strncmp(line, "quill-session 1", 15u) != 0)
    {
        fclose(f);
        return 0u;
    }
    current[0] = 0;
    while (fgets(line, sizeof line, f))
    {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, "current ", 8u) == 0)
        {
            snprintf(current, sizeof current, "%s", line + 8u);
        }
        else if (strcmp(line, "begin") == 0)
        {
            in = true;
            e_path[0] = 0; e_cursor = 0u; e_anchor = 0u; e_top = 0.0f; e_scrollx = 0.0f;
        }
        else if (strncmp(line, "path ", 5u) == 0)
        {
            snprintf(e_path, sizeof e_path, "%s", line + 5u);
        }
        else if (strncmp(line, "cursor ", 7u) == 0) e_cursor = (uint32_t)strtoul(line + 7u, NULL, 10);
        else if (strncmp(line, "anchor ", 7u) == 0) e_anchor = (uint32_t)strtoul(line + 7u, NULL, 10);
        else if (strncmp(line, "top ", 4u) == 0) e_top = (float)strtod(line + 4u, NULL);
        else if (strncmp(line, "scrollx ", 8u) == 0) e_scrollx = (float)strtod(line + 8u, NULL);
        else if (strcmp(line, "end") == 0 && in)
        {
            Document* d;
            in = false;
            if (!e_path[0] || access(e_path, F_OK) != 0) continue; /* gone */
            if (bm->count >= QUILL_MAX_BUFFERS) break;
            d = bm_open(bm, e_path);
            if (!d) continue;
            d->cursor = e_cursor < d->q.text_len ? e_cursor : d->q.text_len;
            d->anchor = e_anchor < d->q.text_len ? e_anchor : d->q.text_len;
            d->view_top = e_top > 0.0f ? e_top : 0.0f;
            d->scroll_x = e_scrollx > 0.0f ? e_scrollx : 0.0f;
            count++;
        }
    }
    fclose(f);

    if (count > 0u)
    {
        bm->current = 0u;
        if (current[0])
            for (i = 0u; i < bm->count; ++i)
                if (strcmp(bm->docs[i]->path, current) == 0) { bm->current = i; break; }
    }
    return count;
}
