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
         "settings.html": "PAGE_SETTINGS", "panel.html": "PAGE_PANEL",
         "widgets.html": "PAGE_WIDGETS"}


def js_literal(fname: str) -> str:
    """foo-bar.js -> PAGE_FOO_BAR_JS"""
    stem = re.sub(r"[^0-9a-zA-Z]+", "_", fname.rsplit(".", 1)[0]).upper()
    return f"PAGE_{stem}_JS"


def discover_js() -> dict:
    """auto-discover <script src="/xxx.js"> tags across the pages: every
    referenced file is embedded and served as application/javascript -
    adding a new js file needs no generator change, only a web.c route"""
    found = {}
    for fname in PAGES:
        for m in re.finditer(r'<script src="/([\w.\-]+\.js)"></script>',
                             (WWW / fname).read_text(encoding="utf-8")):
            found.setdefault(m.group(1), js_literal(m.group(1)))
    return found

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


def js_minified(fname: str) -> str:
    """comment-stripped body of a www js file"""
    js = (WWW / fname).read_text(encoding="utf-8")
    js = re.sub(r"/[*].*?[*]/", "", js, flags=re.S)
    return "\n".join(l for l in js.split("\n")
                     if l.strip() and not l.lstrip().startswith("//"))


# порядок секций единого приложения; секция из модуля filename
# получает id sec-<имя без расширения>
SECTIONS = ["index.html", "widgets.html", "panel.html",
            "settings.html", "wifi.html"]


def scope_css(css: str, sid: str) -> str:
    """
    Prefix every rule of a module with its section id, so merged styles
    stop leaking across sections (each page styled its own controls
    before the SPA merge). Handles plain rules, @media blocks, and the
    body/:root roots (the edit-mode class now lives on the section).
    """
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)   # comments confuse the scanner

    def sel(sc: str) -> str:
        parts = [p.strip() for p in sc.split(",")]
        out = []
        for p in parts:
            if p == ":root" or p == "body" or p == "html":
                out.append(sid)
            elif p.startswith("body.edit"):
                out.append(sid + p[len("body"):])   # .edit ... -> sec.edit
            elif p.startswith("body"):
                out.append(sid + p[4:])
            else:
                out.append(sid + " " + p)
        return ", ".join(out)

    res, i, n = [], 0, len(css)

    def matching_brace(s: str, start: int) -> int:
        """index of the '}' closing the '{' at start (depth-aware)"""
        depth = 0
        for k in range(start, len(s)):
            if s[k] == "{":
                depth += 1
            elif s[k] == "}":
                depth -= 1
                if depth == 0:
                    return k
        return len(s) - 1

    while i < n:
        b = css.find("{", i)
        if b < 0:
            res.append(css[i:])
            break
        selector = css[i:b].strip()
        e = matching_brace(css, b)
        body = css[b + 1:e]
        if selector.startswith("@media"):
            res.append(selector + "{" + scope_css(body, sid) + "}")
        else:
            res.append(sel(selector) + " {" + body + "}")
        i = e + 1
    return "".join(res)

ROUTER = """
/* SPA-роутер: один html, переходы между секциями без подгрузок.
 * Секция = hash (#main/#widgets/#panel/#settings/#wifi); путь
 * /settings и т.п. тоже открывает свою секцию. Видимость - ТОЛЬКО
 * класс .on (CSS ниже), атрибут hidden не используется. */
function routeTo(id) {
  if (location.hash !== '#' + id) location.hash = id;
  else showSec(id);
}
function showSec(id) {
  let found = false;
  document.querySelectorAll('.sec').forEach(s => {
    const on = s.id === 'sec-' + id;
    if (on) found = true;
    s.classList.toggle('on', on);
  });
  if (!found) showSec('main');
  else if (id === 'main') chartDrawAll && chartDrawAll();
}
function spaRoute() {
  const h = location.hash.replace('#', '');
  if (h) return showSec(h);
  const p = location.pathname.replace(/\\/$/, '');
  const map = { '/settings': 'settings', '/panel': 'panel',
                '/widgets': 'widgets', '/wifi': 'wifi' };
  showSec(map[p] || 'main');
}
window.addEventListener('hashchange', spaRoute);
"""


def assemble_app() -> str:
    """
    The single-page application: every module page becomes a <section>
    (hidden except the active one); styles merge into the head; the
    scripts run once in shared scope. Modules stay split in www/ for
    editing - this is a build-time composition.
    """
    styles, sections, scripts = [], [], []
    for fname in SECTIONS:
        name = fname.rsplit(".", 1)[0]
        if name == "index":
            name = "main"      # главная секция = #main
        html = (WWW / fname).read_text(encoding="utf-8")
        st = re.search(r"<style>([\s\S]*?)</style>", html)
        if st:
            styles.append(f"/* --- {fname} --- */\n"
                          + scope_css(st.group(1).strip(), f"#sec-{name}"))
        body = re.search(r"<body>([\s\S]*?)</body>", html)
        body_html = body.group(1).strip()
        # src-теги не попадают в тело: скрипты собираются в один блок ниже
        for m in re.finditer(r'<script src="/([\w.\-]+\.js)"></script>\s*', html):
            body_html = body_html.replace(m.group(0).strip(), "")
            if m.group(1) not in [s[0] for s in scripts]:
                scripts.append((m.group(1), True))
        sections.append(f'<section class="sec" id="sec-{name}">\n'
                        + body_html + "\n</section>")
    ordered = ([s for s in scripts if s[0] == "shared.js"] +
               [s for s in scripts if s[0] != "shared.js"])
    js = "\n;\n".join("/* --- %s --- */\n%s" % (f, js_minified(f))
                      for f, _ in ordered)
    app = ("<!doctype html>\n<html lang=\"ru\">\n<head>\n"
           "<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
           "<title>SKE-02</title>\n<style>\n"
           # shell base: фон/шрифт приложения (стили модулей scoped)
           "body{background:#000;color:#d8dee9;font:13px/1.45 system-ui,sans-serif;"
           "margin:0;padding:4px}\n"
           # мобильный Firefox крупнее Chrome: движковые умолчания
           # контролов и text-size-adjust - нормализуем
           "html{-moz-text-size-adjust:100%;-webkit-text-size-adjust:100%;"
           "text-size-adjust:100%}\n"
           "button,input,select,textarea{font:inherit;color:inherit}\n"
           ":root{color-scheme:dark}\n"
           # видимость секций - только классом .on из роутера
           ".sec{display:none!important}\n"
           ".sec.on{display:block!important}\n"
           + "\n".join(styles) +
           "\n</style>\n</head>\n<body>\n" + "\n".join(sections) +
           "\n<script>\n" + js + "\n;/* --- router --- */\n" + ROUTER +
           "\nspaRoute();\n</script>\n</body>\n</html>\n")
    app = re.sub(r"<!--.*?-->", "", app, flags=re.S)
    app = "\n".join(l for l in app.split("\n") if l.strip())
    (WWW / "app.html").write_text(app, encoding="utf-8", newline="\n")
    return app


def build() -> None:
    """
    Firmware gets ONE composed SPA page (app.html from the split modules):
    a single request, all section switches happen client-side. The mock
    serves the same composed app.html.
    """
    parts = [HEADER]
    app = assemble_app()
    parts.append(f"static const char PAGE_APP[] =\n{c_literal(app)};\n\n")
    # gzip-копия: телефон с энергосбережением тянет 80 КБ по B+G
    # медленно и упирается в send-таймаут — сжатая уходит в разы
    # быстрее. Браузеры Accept-Encoding: gzip понимают все.
    import gzip as _gz
    gz = _gz.compress(app.encode("utf-8"), 9)
    hexarr = ",".join(str(b) for b in gz)
    parts.append(f"static const unsigned char PAGE_APP_GZ[{len(gz)}] =\n{{{hexarr}}};\n\n")
    parts.append("#endif /* GW_PAGES_H */\n")
    HDR.write_text("".join(parts), encoding="utf-8", newline="\n")
    print(f"{HDR} written: single-page app from {', '.join(SECTIONS)} "
          f"(+ {', '.join(sorted(discover_js()))})")


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
