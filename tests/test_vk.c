#include "frame.h"
#include "render_vk.h"
#include <assert.h>

int main(void)
{
    Document d;
    FontSystem* fs;
    DrawList* dl;
    FrameScratch* scratch;
    LineLayout layouts[QUILL_MAX_VISIBLE];
    uint32_t nlayouts;
    FrameParams p;
    VkRenderCreateInfo ci;
    VkRenderer* vk;
    FILE* f;
    static const char SRC[] = "int main(void) { return 0; }\n";
    int i;

    assert(doc_init(&d, 16u << 20));
    assert(doc_set_text(&d, SRC, (uint32_t)strlen(SRC)));
    doc_attach_syntax(&d, syntax_create_for_path("x.c"));
    fs = font_create(16u, 1u);
    dl = (DrawList*)MU_CALLOC(1u, sizeof *dl);
    scratch = (FrameScratch*)MU_CALLOC(1u, sizeof *scratch);
    assert(fs && dl && scratch);

    memset(&ci, 0, sizeof ci);
    ci.headless = true;
    ci.width = 640u;
    ci.height = 480u;
    ci.validation = true;
    vk = vk_render_create(&ci);
    assert(vk);

    memset(&p, 0, sizeof p);
    p.width = 640u; p.height = 480u; p.tab_width = 4u; p.line_numbers = true;
    for (i = 0; i < 3; ++i)
    {
        frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
        assert(vk_render_frame(vk, dl, fs));
    }
    assert(vk_render_resize(vk, 800u, 600u));
    p.width = 800u; p.height = 600u;
    frame_build(&d, fs, &p, dl, scratch, layouts, &nlayouts);
    assert(vk_render_frame(vk, dl, fs));
    assert(vk_render_screenshot(vk, "build/test_vk.png"));
    f = fopen("build/test_vk.png", "rb");
    assert(f);
    fclose(f);

    vk_render_destroy(vk);
    MU_FREE(dl);
    MU_FREE(scratch);
    font_destroy(fs);
    doc_destroy(&d);
    puts("test_vk: PASS");
    return 0;
}
