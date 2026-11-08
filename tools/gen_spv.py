#!/usr/bin/env python3
from pathlib import Path
out=['/* Generated SPIR-V. Do not edit. */\n#ifndef QUILL_SPV_H\n#define QUILL_SPV_H\n#include <stdint.h>\n']
for name in ['solid_vert','solid_frag','glyph_vert','glyph_frag','vfx_vert','vfx_frag','bg_vert','bg_frag']:
    data=Path('src/spv/'+name.replace('_','.')+'.spv').read_bytes()
    words=[int.from_bytes(data[i:i+4],'little') for i in range(0,len(data),4)]
    out.append(f'static const uint32_t {name}[] = {{\n')
    for i in range(0,len(words),8):
        out.append('    '+', '.join(f'0x{w:08x}u' for w in words[i:i+8])+',\n')
    out.append('};\n')
out.append('#endif\n')
Path('src/spv.h').write_text(''.join(out))
