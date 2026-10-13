#!/usr/bin/env python3
"""Embed the standalone HTML UI as a byte-array header."""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
html = (root / 'web' / 'index.html').read_bytes()
dest = root / 'src' / 'web_asset.h'
with dest.open('w', encoding='utf-8') as output:
    output.write('/* Generated from web/index.html; do not hand-edit. */\n#pragma once\n')
    output.write('static const unsigned char gallery_html[] = {\n')
    for i in range(0, len(html), 20):
        output.write('    ' + ','.join(str(n) for n in html[i:i+20]) + ',\n')
    output.write('};\n')
    output.write(f'static const unsigned long gallery_html_size = {len(html)}UL;\n')
print(f'Embedded {len(html)} bytes in {dest.relative_to(root)}')
