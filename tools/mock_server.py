import http.server, json, math, time, random, pathlib

WWW = pathlib.Path(__file__).parent.parent / "www"

# --- 'm' frame simulator ------------------------------------------------
# All MeterData_t members change over time:
#   - rate is one consistent process: frequency <-> rate via the K-factor,
#     rate_fast/rate_raw/rateMLPM are derived views of the same value
#   - totals only grow (integrated from the current rate)
#   - each flag byte flips bits on its OWN period (1..10 s)
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
    # slow working point + medium swings + fast jitter, always >= 0
    base = 12 + 6 * math.sin(t * 0.11) + 3 * math.sin(t * 0.9)
    return max(0.0, base + 0.4 * math.sin(t * 7.3))

def flag_byte(t, group):
    period, bits = FLAGS[group]
    k = int(t / period)
    v = 0
    rnd = random.Random(group + str(k))
    for b in bits:
        if rnd.random() < 0.35:          # a fresh subset each period
            v |= 1 << b
    return v

def values():
    now = time.time()
    t = now - STATE["t0"]
    dt = max(0.0, now - STATE["last"])
    STATE["last"] = now

    rate = rate_now(t)
    freq = rate * KF / 60.0              # Hz: rate L/min * K / 60
    # integrate totals; a small constant "reverse" leak grows t_minus
    STATE["t_plus"] += rate * dt / 60.0
    STATE["t_minus"] += 0.002 * dt
    STATE["gtotal"] = STATE["t_plus"] + STATE["t_minus"]
    STATE["pulses"] += freq * dt
    STATE["batch"] = 2.5 + 1.2 * math.sin(t * 0.05)

    # rateMLPM: independent triangle sweep 1000 -> 1000000 -> 1000 in 50 s
    ph = (t % 6000.0) / 3000.0
    mlpm = 1000.0 + (100000.0 - 1000.0) * (ph if ph <= 1 else 2 - ph)

    tp, tm = STATE["t_plus"], STATE["t_minus"]
    d = {
        "ok": True, "link": True,
        "frequency": freq,
        "rate_raw": rate / KF,           # raw pulse-domain view
        "rate_fast": rate + 0.2 * math.sin(t * 3.1),
        "rateMLPM": mlpm,
        "rate": rate,
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
    return d
# ------------------------------------------------------------------------

class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(WWW), **kw)

    def _json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/values":
            self._json(values())
            return
        if self.path == "/api/info":
            self._json({"link": True, "fw": "SKE02/mock", "count": 0,
                        "ip": "127.0.0.1", "rssi": -50, "ssid": "mock",
                        "uptime": int(time.time() - STATE["t0"]),
                        "heap": 20000, "ap": False})
            return
        super().do_GET()  # static files from www/

    def log_message(self, *a):
        pass

print("serving /api mock on :8099")
http.server.HTTPServer(("127.0.0.1", 8099), H).serve_forever()
