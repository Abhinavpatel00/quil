#!/usr/bin/env python3
from pathlib import Path
langs = [('c','c'),('json','json'),('rust','rust'),('python','python'),('javascript','javascript')]
out = ['/* Generated from third_party/<lang>/highlights.scm. Do not edit. */\n#include "syntax_queries.h"\n\n']
for name, folder in langs:
    text = Path(f'third_party/{folder}/highlights.scm').read_text()
    out.append(f'const char quill_query_{name}[] =\n')
    for line in text.splitlines(True):
        esc = line.replace('\\', '\\\\').replace('"', '\\"').replace('?', '\\?').replace('\n', '\\n')
        out.append(f'"{esc}"\n')
    out.append(';\n\n')
Path('src/syntax_queries.c').write_text(''.join(out).rstrip() + '\n')
