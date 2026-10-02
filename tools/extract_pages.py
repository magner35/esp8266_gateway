#!/usr/bin/env python3
"""Regenerate src/pages.h from the Arduino web.cpp page literals."""
import re

src = open('legacy_arduino/web.cpp', encoding='utf-8').read()


def extract(name):
    m = re.search(name + r'\[\] PROGMEM = R"HTML\((.*?)\)HTML"', src, re.S)
    if not m:
        raise SystemExit(f"{name} not found")
    return m.group(1)


def c_literal(html):
    out = []
    for line in html.split('\n'):
        esc = line.replace('\\', '\\\\').replace('"', '\\"')
        out.append('"' + esc + '\\n"')
    return '\n'.join(out)


def extract_css(name):
    """PAGE_CSS is a plain concatenated string literal, not a raw one."""
    m = re.search(name + r'\[\] PROGMEM =\s*(.*?);', src, re.S)
    if not m:
        raise SystemExit(f"{name} not found")
    body = m.group(1)
    body = body.replace('"', '').replace('\n', '').replace('\\n', '\n')
    return body


pages = {
    'PAGE_WIFI': extract('PAGE_WIFI'),
    'PAGE_CSS': extract_css('PAGE_CSS'),
    'PAGE_INDEX': extract('PAGE_INDEX'),
}

with open('src/pages.h', 'w', encoding='utf-8') as f:
    f.write('/*\n * Web pages, generated from the Arduino version by\n'
            ' * tools/extract_pages.py - do not edit by hand.\n */\n'
            '#ifndef GW_PAGES_H\n#define GW_PAGES_H\n\n')
    for name, html in pages.items():
        f.write('static const char ' + name + '[] =\n' +
                c_literal(html) + ';\n\n')
    f.write('#endif /* GW_PAGES_H */\n')
print('src/pages.h regenerated')
