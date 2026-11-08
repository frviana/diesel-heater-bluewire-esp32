#!/usr/bin/env python3
"""Turn web_src/*.html + core.js into the PROGMEM .h tabs of the ESP32 sketch.
v1.7: pages are stored gzipped (about a quarter of the flash, and faster to load);
the ESP32 sends them with Content-Encoding: gzip, which every browser understands.
v1.9.4: also the home-screen icon (icons/*.png, drawn in icons/icon.svg) -> web_icons.h,
stored as-is (PNG is already compressed), and manifest.json (gzipped like the pages).
Usage: python3 build_pages.py <sketch_dir>"""
import os, sys, gzip

SRC = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SRC, '..', '..', 'firmware', 'heater_web_esp32')
PAGES = [('core.js', 'web_core.h', 'CORE_JS'), ('burner.html', 'web_burner.h', 'PAGE_BURNER'),
         ('lcd.html', 'web_lcd.h', 'PAGE_LCD'), ('dash.html', 'web_dash.h', 'PAGE_DASH'),
         ('hist.html', 'web_hist.h', 'PAGE_HIST'), ('manifest.json', 'web_manifest.h', 'MANIFEST_JSON')]
ICONS = [('icons/icon-192.png', 'ICON_192'), ('icons/icon-512.png', 'ICON_512'), ('icons/icon-48.png', 'ICON_48')]

def emit(f, name, data):
    f.write('const size_t %s_LEN = %d;\n' % (name, len(data)))
    f.write('const uint8_t %s[] PROGMEM = {\n' % name)
    for i in range(0, len(data), 24):
        f.write(','.join('0x%02x' % b for b in data[i:i + 24]) + ',\n')
    f.write('};\n')

def squeeze(text):
    # strip indentation and blank lines; keep everything else as written
    return '\n'.join(l.strip() for l in text.splitlines() if l.strip()) + '\n'

total = ztotal = 0
for src, dst, name in PAGES:
    body = squeeze(open(os.path.join(SRC, src), encoding='utf-8').read()).encode('utf-8')
    z = gzip.compress(body, compresslevel=9, mtime=0)
    total += len(body); ztotal += len(z)
    with open(os.path.join(OUT, dst), 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generated from web_src/%s by build_pages.py - edit the source, not this file.\n' % src)
        f.write('// gzip of %d bytes of page text\n#pragma once\n#include <pgmspace.h>\n' % len(body))
        emit(f, name, z)
    print('%-12s -> %-13s %6d bytes, %6d gzipped' % (src, dst, len(body), len(z)))
print('total %d bytes, %d gzipped' % (total, ztotal))
itotal = 0
with open(os.path.join(OUT, 'web_icons.h'), 'w', encoding='utf-8', newline='\n') as f:
    f.write('// Generated from web_src/icons/*.png by build_pages.py - the home-screen icon.\n')
    f.write('// Drawn in web_src/icons/icon.svg. PNGs are stored as-is.\n#pragma once\n#include <pgmspace.h>\n')
    for src, name in ICONS:
        data = open(os.path.join(SRC, src), 'rb').read()
        itotal += len(data)
        emit(f, name, data)
        print('%-20s -> web_icons.h   %6d bytes' % (src, len(data)))
print('icons %d bytes' % itotal)
