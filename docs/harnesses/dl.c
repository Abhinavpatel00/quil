#include <stdio.h>
#include "quill.h"
int main(void){ printf("  With QUILL_MAX_GLYPHS=%u: glyph ring %u*6*32 = %.2f MiB\n", QUILL_MAX_GLYPHS,
  QUILL_MAX_GLYPHS, QUILL_MAX_GLYPHS*6.0*32/1048576.0); return 0; }
