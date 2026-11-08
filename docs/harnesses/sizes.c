#include "document.h"
#include "frame.h"
#include "modal.h"
#include "vfx.h"
#include "font.h"
#include "picker.h"
#include "buffers.h"
#include "terminal.h"
#include <stdio.h>
int main(void){
#define P(T) printf("%-22s %10zu bytes  %7.3f MiB\n", #T, sizeof(T), sizeof(T)/1048576.0)
    P(SolidQuad);P(GlyphQuad);P(VfxQuad);P(HighlightSpan);
    P(DrawList);P(FrameScratch);P(LineLayout);P(ModalState);P(VfxState);
    P(Picker);P(Document);P(BufferManager);P(SelectionSet);
    printf("\n-- sub-parts --\n");
    printf("DrawList.solids      %8zu (%u x %zu)\n", sizeof(SolidQuad)*QUILL_MAX_SOLIDS, QUILL_MAX_SOLIDS, sizeof(SolidQuad));
    printf("DrawList.glyphs      %8zu (%u x %zu)\n", sizeof(GlyphQuad)*QUILL_MAX_GLYPHS, QUILL_MAX_GLYPHS, sizeof(GlyphQuad));
    printf("DrawList.vfx         %8zu (%u x %zu)\n", sizeof(VfxQuad)*QUILL_MAX_VFX, QUILL_MAX_VFX, sizeof(VfxQuad));
    printf("ModalState.regs      %8zu (%u x %u)\n", (size_t)QUILL_MAX_REGISTERS*QUILL_REG_CAP, QUILL_MAX_REGISTERS, QUILL_REG_CAP);
    printf("FrameScratch.spans   %8zu\n", sizeof(HighlightSpan)*QUILL_MAX_SPANS);
    printf("BufferManager.recent %8zu\n", (size_t)QUILL_MAX_BUFFERS*512);
    return 0;
}
