#!/usr/bin/env python3
"""Convert the plain HTML files from www/ into the C string literals of
src/pages.h (and back). The www/ files are the hand-edited source of truth:

    python tools/pages.py build     # www/*.html  -> src/pages.h
    python tools/pages.py extract   # src/pages.h -> www/*.html (overwrites)

Each page maps to a literal named after its file upper-cased without the
extension (wifi.html -> PAGE_WIFI, index.html -> PAGE_INDEX).

The generator also runs automatically before every PlatformIO build
(extra_scripts = pre:tools/pages_build.py), so editing www/*.html is
enough - no manual step.
"""
import argparse
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
WWW = ROOT / "www"
HDR = ROOT / "src" / "pages.h"

PAGES = {"wifi.html": "PAGE_WIFI", "index.html": "PAGE_INDEX",
         "settings.html": "PAGE_SETTINGS"}

HEADER = """/*
 * Web pages - AUTO-GENERATED from the plain HTML files in the www dir.
 * Do not edit by hand: edit the html files there, then run
 *     python tools/pages.py build
 */
#ifndef GW_PAGES_H
#define GW_PAGES_H

"""

# placeholder keeping escaped backslashes out of the later replacements
_BK = "\x00"


def minify(html: str) -> str:
    """Drop comments and blank lines from <style>/<script> bodies before
    packing into pages.h - the www/ sources keep their comments, the
    flash image does not need them.

    Conservative on purpose: block comments /* .. */ (none of the pages
    has these sequences inside string literals) and whole-line //
    comments only; inline // is left alone so '//host' in strings stays.
    """
    def strip(body: str, css: bool) -> str:
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        if not css:
            body = "\n".join(
                l for l in body.split("\n")
                if l.strip() and not l.lstrip().startswith("//"))
        else:
            body = "\n".join(l for l in body.split("\n") if l.strip())
        return body

    out, pos = [], 0
    for m in re.finditer(r"<(style|script)>(.*?)</\1>", html, re.S):
        out.append(html[pos:m.start()])
        tag = m.group(1)
        out.append(f"<{tag}>" + strip(m.group(2), tag == "style") +
                   f"</{tag}>")
        pos = m.end()
    out.append(html[pos:])
    return "".join(out)


def c_literal(html: str) -> str:
    """HTML source -> C concatenated string literal.

    Only three escapes exist in the literal: \n (newline), \" (quote) and
    \\\\ (backslash). Backslash MUST be escaped first, otherwise a JS
    '\\x01' would turn into a double backslash and break the page.
    """
    out = []
    lines = html.split("\n")
    for i, line in enumerate(lines):
        if i + 1 == len(lines) and line == "":
            break  # file ended with a newline - no empty last fragment
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'"{esc}\\n"')
    return "\n".join(out)


def build() -> None:
    parts = [HEADER]
    for fname, lit in PAGES.items():
        html = minify((WWW / fname).read_text(encoding="utf-8"))
        parts.append(f"static const char {lit}[] =\n{c_literal(html)};\n\n")
    parts.append("#endif /* GW_PAGES_H */\n")
    HDR.write_text("".join(parts), encoding="utf-8", newline="\n")
    print(f"{HDR} written from: {', '.join(PAGES)}")


def extract() -> None:
    src = HDR.read_text(encoding="utf-8")
    for fname, lit in PAGES.items():
        m = re.search(lit + r"\[\] =\n(.*?)\;\n", src, re.S)
        if not m:
            raise SystemExit(f"{lit} not found in pages.h")
        body = ""
        for line in m.group(1).splitlines():
            t = line.strip()
            if t.startswith('"') and t.endswith('"'):
                body += t[1:-1]
        # unescape in the safe order: escaped backslash first (sentinel),
        # then the single-character escapes, then restore the sentinel
        body = body.replace("\\\\", _BK).replace("\\n", "\n").replace(
            '\\"', '"').replace(_BK, "\\")
        (WWW / fname).write_text(body, encoding="utf-8", newline="\n")
        print(f"{fname} written, {len(body)} chars")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["build", "extract"])
    args = ap.parse_args()
    build() if args.cmd == "build" else extract()


if __name__ == "__main__":
    main()
