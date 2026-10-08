import http.server, json, math, os, time, random, pathlib, re, urllib.parse, socket, threading, collections

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
_DUMP_MTIME = DUMP.stat().st_mtime

def dump_maybe_reload():
    """hot-reload tools/ske_l.txt: reparse when its mtime changes.
    Runtime /api/set edits to values are reset by a reload - the dump
    is the source of truth while iterating on the parser/UI."""
    global SECTIONS, PARAMS, COUNT, _DUMP_MTIME
    try:
        m = DUMP.stat().st_mtime
    except OSError:
        return
    if m != _DUMP_MTIME:
        _DUMP_MTIME = m
        SECTIONS, PARAMS = parse_dump()
        COUNT = max(PARAMS) + 1
        print(f"dump reloaded: {COUNT} params, {len(SECTIONS)} sections", flush=True)

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
    # под выборку 10 Гц, частоты вдвое ниже прежних:
    # 0.04/0.24/0.95/2.4 Гц — плавная волна даже на длинных окнах
    return max(0.0, 12 + 4 * math.sin(t * 0.25)
               + 3 * math.sin(t * 1.5)
               + 2 * math.sin(t * 6.0)
               + 0.5 * math.sin(t * 15.0))

def flag_byte(t, group):
    period, bits = FLAGS[group]
    rnd = random.Random(group + str(int(t / period)))
    v = 0
    for b in bits:
        if rnd.random() < 0.35:
            v |= 1 << b
    return v

def _enum_val(pid, default):
    p = PARAMS.get(pid)
    return p['v'] if p else default

def unit_ml(pid):
    """ml per the chosen volume unit (params 54/55/56)"""
    return {'Миллилитр': 1.0, 'Литр': 1000.0, 'Метр куб.': 1e6}.get(
        _enum_val(pid, 'Литр'), 1000.0)

def per_sec():
    """seconds of the chosen rate period (param 53)"""
    return {'Секунда': 1.0, 'Минута': 60.0, 'Час': 3600.0}.get(
        _enum_val(53, 'Минута'), 60.0)

CHART_HIST = collections.deque(maxlen=512)   # (t_ms, rate) как chartbuf на ESP

def _chart_sampler():
    """1 Гц независимо от браузера — как meter_task на ESP."""
    while True:
        t = time.time() - STATE["t0"]
        # то же число, что values() кладёт в "rate"
        CHART_HIST.append((int(time.time() * 1000),
                           rate_now(t) * (1000.0 / unit_ml(54)) * per_sec() / 60.0))
        time.sleep(1.0)

VAL_RING = collections.deque(maxlen=8)   # кольцо снимков, как ske02_valring

def _values_sampler():
    """5 Гц независимо от браузера: эволюция модели + снимок в кольцо."""
    while True:
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
        ph = (t % 6000.0) / 3000.0
        mlpm = 1000.0 + (100000.0 - 1000.0) * (ph if ph <= 1 else 2 - ph)
        tp, tm = STATE["t_plus"], STATE["t_minus"]
        ur = 1000.0 / unit_ml(54)
        uo = 1000.0 / unit_ml(55)
        ug = 1000.0 / unit_ml(56)
        ps = per_sec()
        VAL_RING.append({
            "t": int(now * 1000),
            "frequency": freq,
            "rate_raw": rate / KF,
            "rate_fast": (rate + 0.2 * math.sin(t * 3.1)) * ur * ps / 60.0,
            "rateMLPM": mlpm, "rate": rate * ur * ps / 60.0,
            "total_plus": tp * uo, "total_minus": tm * uo,
            "total": (tp - tm) * uo, "total_sum": (tp + tm) * uo,
            "totalml_plus": tp * 1000.0, "totalml_minus": tm * 1000.0,
            "gtotal": STATE["gtotal"] * ug, "gtotalml": STATE["gtotal"] * 1000.0,
            "kf_value": KF,
            "pulses_packet": int(freq) & 0xFFFF,
            "pulses": int(STATE["pulses"]),
            "batch": STATE["batch"] * uo,
            "status": flag_byte(t, "status"),
            "setpoint": flag_byte(t, "setpoint"),
            "isr": flag_byte(t, "isr"),
        })
        time.sleep(0.2)

def values(since=0):
    now = time.time()
    ap = bool(os.environ.get("MOCK_AP"))
    seq = [x for x in VAL_RING if x["t"] > since]
    if since == 0:
        seq = seq[-1:]       # загрузка страницы: только свежий снимок
    d = {
        "ok": True, "link": True, "ready": True,
        "now": int(now * 1000),
        "ap": ap,
        "ip": "10.0.0.1" if ap else "127.0.0.1",
        "ssid": "ske02setup-TEST" if ap else "mock",
        "seq": seq,
    }
    return d
# ------------------------------------------------------------------------

class H(http.server.SimpleHTTPRequestHandler):
    WCFG = {"rev": 0, "cfg": None}   # NVS stand-in, survives page reloads

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(WWW), **kw)

    def end_headers(self):
        """no-store на ВСЁ: webview кэширует /index.js и после
        пересборки страница исполняет старый скрипт"""
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # SPA: все страницы отдают скомпонованный app.html (роутер внутри)
    PAGES = {p: "app.html" for p in
             ["/", "/index.html", "/settings", "/wifi", "/panel", "/widgets"]}

    def do_GET(self):
        self._path = urllib.parse.urlparse(self.path).path
        if self._path == "/api/values":
            qs = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            self._json(values(since=int(qs.get("since", ["0"])[0])))
        elif self._path == "/api/info":
            ap = bool(os.environ.get("MOCK_AP"))
            self._json({"link": True, "fw": "SKE02/3425", "count": COUNT,
                        "ip": "10.0.0.1" if ap else "127.0.0.1",
                        "rssi": -50 if not ap else 0,
                        "ssid": "ske02setup-TEST" if ap else "mock",
                        "uptime": int(time.time() - STATE["t0"]),
                        "heap": 20000, "ap": ap})
        elif self._path == "/api/params":
            dump_maybe_reload()
            self._json(params_json())
        elif self.path.startswith("/api/chart"):
            qs = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            since = int(qs.get("since", ["0"])[0])
            pts = [(t, v) for (t, v) in CHART_HIST if t > since]
            self._json({"now": int(time.time() * 1000), "n": len(pts),
                        "t": [t for t, _ in pts], "v": [round(v, 4) for _, v in pts]})
        elif self.path.startswith("/scan"):
            self._json({"nets": [
                {"s": "CALIPSO1703", "r": -42, "e": 1},
                {"s": "DIR-320-Home", "r": -58, "e": 1},
                {"s": "Keenetic-911", "r": -71, "e": 1},
                {"s": "FreeWiFi", "r": -85, "e": 0}
            ]})
        elif self._path == "/api/widgets":
            self._json(self.WCFG)
        elif self._path == "/stamp":
            self._json({"t": www_stamp()})
        elif self._path in self.PAGES:
            # serve the page with an auto-reload probe injected (mock only,
            # never in pages.h): saving any www/*.html reloads the browser
            html = (WWW / self.PAGES[urllib.parse.urlparse(self.path).path]).read_text(encoding='utf-8')
            # версионирование скриптов: браузеры кэшируют /xx.js
            # эвристически и гоняют СТАРЫЙ код после пересборки
            import re as _re
            st = www_stamp()
            html = _re.sub(r'src="/([a-z]+\.js)"',
                           lambda m: 'src="/%s.js?v=%d"' % (m.group(1), st), html)
            # захват JS-ошибок страницы для диагностики
            html = html.replace('<head>',
                '<head><script>window.__errs=[];window.onerror='
                '(m,s,l)=>{window.__errs.push(m+" @"+l)};</script>', 1)
            html = html.replace('</body>', RELOAD_JS + '</body>')
            body = html.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            super().do_GET()

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n).decode() if n else ''
        f = dict(urllib.parse.parse_qsl(body))
        if self.path == "/api/forgetwifi":
            self._json({"ok": True})
        elif self.path == "/api/set":
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
        elif self._path == "/api/widgets":
            try:
                d = json.loads(body)
                assert 'rev' in d and isinstance(d.get('cur') or d.get('cfg'), list)
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

    def end_headers(self):
        # телефон не должен сидеть на старых html/js после правок
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def log_message(self, *a):
        pass


def www_stamp():
    """newest mtime across the www/ html files - the reload marker"""
    return max((p.stat().st_mtime for p in WWW.glob('*.html')), default=0.0)


RELOAD_JS = ("<script>setInterval(async()=>{try{const r=await"
             "(await fetch('/stamp')).json();if(window.__st&&r.t!==window.__st)"
             "location.reload();window.__st=r.t}catch(e){}},1000)</script>")

print(f"mock: {COUNT} params, {len(SECTIONS)} sections, on :8099", flush=True)
def lan_ips():
    """все IPv4 машины; первым - адрес основного маршрута (для телефона)"""
    ips = []
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 1))          # не шлёт ничего, только выбирает маршрут
        ips.append(sock.getsockname()[0])
    except OSError:
        pass
    finally:
        sock.close()
    try:
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if ip not in ips and not ip.startswith("127."):
                ips.append(ip)
    except OSError:
        pass
    return ips


PORT = 8099
print(f"mock: {COUNT} params, {len(SECTIONS)} sections, on :{PORT}", flush=True)
for ip in lan_ips():
    virtual = ip.startswith(("192.168.56.", "172.1", "172.2", "172.3"))
    tag = " (виртуальный адаптер)" if virtual else " (реальная сеть - подходит для телефона)"
    print(f"  http://{ip}:{PORT}{tag}", flush=True)
# 0.0.0.0: мок доступен и с телефона в той же Wi-Fi сети
threading.Thread(target=_values_sampler, daemon=True).start()
threading.Thread(target=_chart_sampler, daemon=True).start()
http.server.ThreadingHTTPServer(("0.0.0.0", PORT), H).serve_forever()
