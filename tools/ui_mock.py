#!/usr/bin/env python3
"""Offline mock of the gateway web UI.

Serves the dashboard page extracted verbatim from src/web.cpp and fakes
the /api endpoints from a real captured device listing, so UI changes
can be checked in a desktop browser before flashing the ESP.

    python tools/ui_mock.py [port]   # default 8077
"""

import json
import os
import re
import struct
import sys
import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ske02_sim import Db  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, "..", "src", "web.cpp")
LISTING = os.path.join(HERE, "ske_l.txt")

# Same word-wise homoglyph restore as src/ske02.cpp rusifyText(): the
# console emits Cyrillic letters sharing an HD44780 glyph with Latin as
# Latin; convert them back inside words that already hold real Cyrillic.
HOMO = {'A': 'А', 'B': 'В', 'E': 'Е', 'K': 'К', 'M': 'М', 'H': 'Н',
        'O': 'О', 'P': 'Р', 'C': 'С', 'T': 'Т', 'X': 'Х', 'b': 'Ь',
        'a': 'а', 'e': 'е', 'o': 'о', 'p': 'р', 'c': 'с', 'y': 'у', 'x': 'х'}


def rusify(text: str) -> str:
    out, word = [], []

    def flush():
        has_cyr = any('\u0400' <= ch <= '\u04FF' for ch in word)
        out.extend(HOMO.get(ch, ch) if has_cyr else ch for ch in word)
        word.clear()

    for ch in text:
        if ch.isalnum() or ord(ch) >= 0x80:
            word.append(ch)
        else:
            flush()
            out.append(ch)
    flush()
    return ''.join(out)


def extract_page():
    src = open(WEB, encoding="utf-8").read()
    m = re.search(r'PAGE_INDEX\[\] PROGMEM = R"HTML\((.*?)\)HTML"', src, re.S)
    if not m:
        sys.exit("PAGE_INDEX not found in src/web.cpp")
    return m.group(1)


def fmt_value(p, raw):
    t = p["t"]
    if t in (10, 11) and p["opts"]:
        return p["opts"][min(raw, len(p["opts"]) - 1)]
    if t == 0:
        f = struct.unpack("<f", struct.pack("<I", raw))[0]
        s = f"{f:.3f}".rstrip("0").rstrip(".")
        return s or "0"
    if t == 2:
        v = raw % 86400
        return f"{v // 3600:02d}:{v % 3600 // 60:02d}"
    if t == 3:
        d = datetime.date(1970, 1, 1) + datetime.timedelta(days=raw // 86400)
        return f"{d.day:02d}.{d.month:02d}.{d.year % 100:02d}"
    if t == 5:
        return str(struct.unpack("<b", struct.pack("<B", raw & 0xFF))[0])
    if t == 9:
        return str(struct.unpack("<h", struct.pack("<H", raw & 0xFFFF))[0])
    return str(raw)


def fmt_bound(p, which):
    if not p["rng"]:
        return ""
    v = p["rng"][1] if which else p["rng"][0]
    if p["t"] == 0:
        v = struct.unpack("<I", struct.pack("<f", v))[0]
    return fmt_value(p, v)


def build_api():
    text = rusify(open(LISTING, "rb").read().decode("utf-8", "replace"))
    db = Db(text)
    sections, params = [], []
    cur_sec = -1
    hdr = {}  # level -> name of the last header at that level
    for line in text.splitlines():
        line = line.rstrip("\r\n")
        if line.startswith("[L") and "]" in line:
            path = line.split("]", 1)[1].strip()
            parts = path.split("/")
            lvl = len(parts) - 1
            hdr[lvl] = parts[-1]
            if len(parts) >= 2 and parts[1] not in sections:
                sections.append(parts[1])
            cur_sec = sections.index(parts[1]) if len(parts) >= 2 else -1
            continue
        ind = len(line) - len(line.lstrip(" "))
        s = line.lstrip()
        if not s[:1].isdigit():
            continue
        try:
            pid_s, token, rest = s.split(" ", 2)
            pid = int(pid_s)
            p = db.params[pid]
        except (ValueError, KeyError):
            continue
        owner = ind // 2 - 1 if ind >= 2 else 0
        # sanitize like the gateway: control chars (LCD specials) -> space
        name = "".join(ch if ord(ch) >= 32 else " " for ch in p["name"]).strip()
        params.append({
            "i": pid, "t": p["t"], "r": p["raw"],
            "n": name, "s": cur_sec,
            "g": hdr.get(owner, ""), "gl": max(owner, 0),
            "tb": hdr.get(2, "") if owner >= 2 else "",
            "v": fmt_value(p, p["raw"]),
            "w": 0 if p["ro"] else 1,
            "cx": 0,
            "m": 1 if pid == 46 else 0,  # «Пароль» = masked U32
            "lo": fmt_bound(p, 0), "hi": fmt_bound(p, 1),
            "o": p["opts"] or [], "a": 3,
        })
    # synthetic menu command item (the real firmware lists them as
    # "<id> CMD <name>"; the captured listing predates that)
    params.append({
        "i": db.count, "t": 14, "r": 0, "n": "Сброс объёма (команда)",
        "s": 1, "g": "Сброс объёма", "gl": 2, "tb": "Сброс объёма",
        "v": "", "w": 0, "cx": 1, "m": 0, "lo": "", "hi": "", "o": [], "a": 0,
    })
    return {
        "count": db.count + 1,
        "sections": sections,
        "params": params,
        "rc": sum(1 for p in params if p["lo"] or p["o"]),
    }


API = build_api()
DB_BY_ID = {}
for _pid, _p in Db(rusify(open(LISTING, "rb").read().decode("utf-8", "replace"))).params.items():
    DB_BY_ID[_pid] = _p
INFO = {"fw": "SKE02/3425 (mock)", "count": API["count"], "link": 1,
        "ready": 1, "ap": 0, "ssid": "mock", "ip": "127.0.0.1",
        "rssi": -55, "uptime": 1234, "heap": 30000, "poll": 2000}
PAGE = extract_page()


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        b = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            self._send(200, PAGE, "text/html")
        elif self.path == "/api/info":
            INFO["uptime"] += 2
            self._send(200, json.dumps(INFO))
        elif self.path == "/api/values":
            self._send(200, json.dumps({
                "ok": 1, "age": 0,
                "frequency": 25.4, "rate_raw": 12.51, "rate_fast": 12.49,
                "rateMLPM": 12500, "rate": 12.5,
                "total_plus": 123.456, "total_minus": 0.5, "total": 122.956,
                "total_sum": 123.956, "totalml_plus": 123456.2,
                "totalml_minus": 500, "gtotal": 9876.5, "gtotalml": 9876543,
                "kf_value": 1.0, "batch": 10.25,
                "pulses_packet": 4, "pulses": 123456,
                "status": 0x45, "setpoint": 0x0A, "isr": 0x81}))
        elif self.path == "/api/params":
            self._send(200, json.dumps(API))
        else:
            self._send(404, "{}")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode("utf-8")
        q = parse_qs(body or self.path.split("?", 1)[-1])
        # the mock password: anything else is rejected like the device does
        pw = q.get("pw", [""])[0]
        if pw and pw != "1234":
            self._send(400, json.dumps(
                {"ok": False, "error": "нет доступа: неверный пароль меню"}))
            return
        if self.path.startswith("/api/run"):
            pid = int(q.get("id", ["-1"])[0])
            p = next((x for x in API["params"] if x["i"] == pid), None)
            if p and p.get("cx"):
                self._send(200, '{"ok":true}')
            else:
                self._send(400, json.dumps(
                    {"ok": False, "error": "нет такой команды"}))
            return
        if self.path.startswith("/api/set"):
            pid = int(q.get("id", ["-1"])[0])
            p = next((x for x in API["params"] if x["i"] == pid), None)
            if p:
                try:
                    if p["t"] in (10, 11):
                        p["r"] = int(q["v"][0])
                    elif p["t"] == 0:
                        p["r"] = struct.unpack("<I", struct.pack("<f", float(q["v"][0])))[0]
                    else:
                        p["r"] = int(float(q["v"][0].replace(":", ".").split(".")[0])
                                     if p["t"] in (2, 3) else float(q["v"][0]))
                except (ValueError, KeyError):
                    pass
                dbp = DB_BY_ID.get(pid)
                if dbp:
                    p["v"] = fmt_value(dbp, p["r"])
                value = "******" if p.get("m") else p["v"]
                self._send(200, json.dumps({"ok": True, "value": value}))
            else:
                self._send(400, json.dumps({"ok": False, "error": "нет такого параметра"}))
        else:
            self._send(200, '{"ok":true}')

    def do_HEAD(self):
        self._send(200, "{}")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8077
    print(f"ui mock on http://127.0.0.1:{port}/ "
          f"({API['count']} params, sections {API['sections']})")
    HTTPServer(("127.0.0.1", port), H).serve_forever()
