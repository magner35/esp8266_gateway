import http.server, json, math, time, random, pathlib, re, urllib.parse

ROOT = pathlib.Path(__file__).parent.parent
WWW = ROOT / "www"
DUMP = ROOT / "tools" / "ske_l.txt"

# --- 'l' dump -> /api/params JSON (mirrors src/protocol.c + web.c) ------
HOMO = {'A': 'А', 'B': 'В', 'E': 'Е', 'K': 'К', 'M': 'М', 'H': 'Н', 'O': 'О',
        'P': 'Р', 'C': 'С', 'T': 'Т', 'X': 'Х', 'b': 'ь', 'a': 'а', 'e': 'е',
        'o': 'о', 'p': 'р', 'c': 'с', 'y': 'у', 'x': 'х'}
TYPES = {'F': 0, 'U32': 1, 'TIME': 2, 'DATE': 3, 'U8': 4, 'I8': 5,
         'U16': 7, 'I16': 9, 'B': 10, 'E': 11, 'S': 12, 'CMD': 14}

def rusify(s):
    """same homoglyph restore as proto_rusify(): inside a word that already
    contains real Cyrillic, Latin lookalikes become Cyrillic"""
    out, i, n = [], 0, len(s)
    while i < n:
        if (s[i].isascii() and s[i].isalnum()) or ord(s[i]) >= 0x80:
            j = i
            while j < n and ((s[j].isascii() and s[j].isalnum()) or ord(s[j]) >= 0x80):
                j += 1
            word = s[i:j]
            if any(ord(c) >= 0x80 for c in word):
                word = ''.join(HOMO.get(c, c) for c in word)
            out.append(word)
            i = j
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)

def parse_dump():
    sections, sec_idx = [], {}
    hdr = {}                       # menu level -> rusified menu name
    cur_sec = 0
    params = {}
    for line in DUMP.read_text(encoding='utf-8').split('\n'):
        line = re.sub(r'[\x00-\x1f]', '', line).rstrip()
        if not line:
            continue
        m = re.match(r'^\[L(\d+)\] (.*)$', line)
        if m:
            comps = rusify(m.group(2)).split('/')
            hdr[int(m.group(1))] = comps[-1]
            if len(comps) > 1:     # L1 section = the second path component
                sec = comps[1]
                if sec not in sec_idx:
                    sec_idx[sec] = len(sections)
                    sections.append(sec)
                cur_sec = sec_idx[sec]
            continue
        m = re.match(r'^(\s*)(\d+) (\S+)\s+(.*)$', line)
        if not m:
            continue
        ind, pid, tok, rest = m.groups()
        owner = len(ind) // 2 - 1 if len(ind) >= 2 else 0
        t = TYPES.get(tok, 14)
        ro = rest.endswith(' *')
        rest = re.sub(r'[\s*~]+$', '', rest)
        p = {'i': int(pid), 't': t, 'r': 0, 'n': '', 's': cur_sec, 'g': '',
             'gl': owner, 'tb': '', 'v': '', 'w': 0,
             'cx': 1 if t == 14 else 0, 'm': 0, 'lo': '', 'hi': '', 'o': None}
        p['g'] = hdr.get(owner, hdr.get(1, ''))
        p['tb'] = hdr.get(2, '') if owner >= 2 else ''
        if t == 14:
            p['n'] = rusify(rest)
            params[p['i']] = p
            continue
        name, _, val = rest.partition(' = ')
        p['n'] = rusify(name)
        rb = val.rfind(' [')
        if rb >= 0 and val.endswith(']'):
            rng = val[rb + 2:-1]
            val = val[:rb].rstrip()
        else:
            rng = None
        p['v'] = rusify(val)
        if rng and '|' in rng:
            p['o'] = [rusify(o) for o in rng.split('|')]
            p['r'] = p['o'].index(p['v']) if p['v'] in p['o'] else 0
        elif rng and '..' in rng:
            lo, hi = rng.split('..')
            p['lo'] = lo.strip()
            p['hi'] = hi.strip()
        if p['v'] == '******':
            p['m'] = 1
        p['w'] = 0 if ro or t in (12, 14) else 1
        params[p['i']] = p
    return sections, params

SECTIONS, PARAMS = parse_dump()
COUNT = max(PARAMS) + 1

def params_json():
    lst = []
    for i in range(COUNT):
        p = PARAMS.get(i)
        if p is None:
            p = {'i': i, 't': 14, 'r': 0, 'n': '', 's': 0, 'g': '', 'gl': 0,
                 'tb': '', 'v': '', 'w': 0, 'cx': 1, 'm': 0, 'lo': '', 'hi': '',
                 'o': None}
        lst.append(p)
    return {'count': COUNT, 'sections': SECTIONS, 'params': lst,
            'rc': len(lst)}

# --- 'm' frame simulator ------------------------------------------------
KF = 1250.0            # pulses per liter, like the device default
STATE = {"t0": time.time(), "last": time.time(),
         "t_plus": 1234.5, "t_minus": 12.3, "gtotal": 5000.0,
         "pulses": 1540000, "batch": 2.5}

# flag group -> (period s, which bits may toggle)
FLAGS = {
    "setpoint": (2.3, [0, 1, 2, 3, 4, 5, 6, 7]),
    "status":   (5.7, [0, 2, 4, 5, 6, 7]),
    "isr":      (9.4, [0, 1, 2, 3, 5, 6]),
}

def rate_now(t):
    base = 12 + 6 * math.sin(t * 0.11) + 3 * math.sin(t * 0.9)
    return max(0.0, base + 0.4 * math.sin(t * 7.3))

def flag_byte(t, group):
    period, bits = FLAGS[group]
    rnd = random.Random(group + str(int(t / period)))
    v = 0
    for b in bits:
        if rnd.random() < 0.35:
            v |= 1 << b
    return v

def values():
    now = time.time()
    t = now - STATE["t0"]
    dt = max(0.0, now - STATE["last"])
    STATE["last"] = now
    rate = rate_now(t)
    freq = rate * KF / 60.0
    STATE["t_plus"] += rate * dt / 60.0
    STATE["t_minus"] += 0.002 * dt
    STATE["gtotal"] = STATE["t_plus"] + STATE["t_minus"]
    STATE["pulses"] += freq * dt
    STATE["batch"] = 2.5 + 1.2 * math.sin(t * 0.05)
    # rateMLPM: independent triangle sweep 1000 -> 100000 -> 1000 in 50 s
    ph = (t % 6000.0) / 3000.0
    mlpm = 1000.0 + (100000.0 - 1000.0) * (ph if ph <= 1 else 2 - ph)
    tp, tm = STATE["t_plus"], STATE["t_minus"]
    return {
        "ok": True, "link": True,
        "frequency": freq,
        "rate_raw": rate / KF,
        "rate_fast": rate + 0.2 * math.sin(t * 3.1),
        "rateMLPM": mlpm, "rate": rate,
        "total_plus": tp, "total_minus": tm,
        "total": tp - tm, "total_sum": tp + tm,
        "totalml_plus": tp * 1000.0, "totalml_minus": tm * 1000.0,
        "gtotal": STATE["gtotal"], "gtotalml": STATE["gtotal"] * 1000.0,
        "kf_value": KF,
        "pulses_packet": int(freq) & 0xFFFF, "pulses": int(STATE["pulses"]),
        "batch": STATE["batch"],
        "status": flag_byte(t, "status"),
        "setpoint": flag_byte(t, "setpoint"),
        "isr": flag_byte(t, "isr"),
    }
# ------------------------------------------------------------------------

class H(http.server.SimpleHTTPRequestHandler):
    WCFG = {"rev": 0, "cfg": None}   # NVS stand-in, survives page reloads

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(WWW), **kw)

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    PAGES = {"/": "index.html", "/settings": "settings.html",
             "/wifi": "wifi.html"}

    def do_GET(self):
        if self.path == "/api/values":
            self._json(values())
        elif self.path == "/api/info":
            self._json({"link": True, "fw": "SKE02/3425", "count": COUNT,
                        "ip": "127.0.0.1", "rssi": -50, "ssid": "mock",
                        "uptime": int(time.time() - STATE["t0"]),
                        "heap": 20000, "ap": False})
        elif self.path == "/api/params":
            self._json(params_json())
        elif self.path == "/api/widgets":
            self._json(self.WCFG)
        elif self.path in self.PAGES:
            self.path = "/" + self.PAGES[self.path]
            super().do_GET()
        else:
            super().do_GET()

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n).decode() if n else ''
        f = dict(urllib.parse.parse_qsl(body))
        if self.path == "/api/set":
            p = PARAMS.get(int(f.get('id', -1)))
            if p is None or not p['w']:
                self._json({"ok": False, "error": "parametr nedostupen"}, 400)
                return
            v = f.get('v', '')
            p['v'] = v
            if p['o'] and v in p['o']:
                p['r'] = p['o'].index(v)
            self._json({"ok": True, "value": v})
        elif self.path == "/api/run":
            self._json({"ok": True})
        elif self.path == "/api/widgets":
            try:
                d = json.loads(body)
                assert isinstance(d.get('cfg'), list) and 'rev' in d
            except Exception:
                self._json({"ok": False}, 400)
                return
            type(self).WCFG = d
            self._json({"ok": True})
        elif self.path in ("/api/refresh", "/api/rescan", "/api/reboot"):
            self._json({"ok": 1, "ready": 1})
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *a):
        pass

print(f"mock: {COUNT} params, {len(SECTIONS)} sections, on :8099")
http.server.ThreadingHTTPServer(("127.0.0.1", 8099), H).serve_forever()
