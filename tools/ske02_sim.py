#!/usr/bin/env python3
"""SKE-02 service console simulator for gateway bench tests.

Stands in for the flow meter on the other end of the gateway UART and
speaks the real protocol: binary PING/READ/WRITE/READ_MANY/REBOOT/MENU/
RANGE plus the text 'l' listing, with the console echo enabled. The
parameter database (names, sections, ranges, enum options, values) is
parsed from a listing captured from a real device, so the frame formats
match the firmware byte for byte.

    python tools/ske02_sim.py COM28 [--listing path/to/ske_l.txt]
"""

import argparse
import datetime
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("нужен pyserial: pip install pyserial")

SOF = 0xAA
BC = dict(PING=1, READ=2, WRITE=3, READ_MANY=4, REBOOT=5, MENU=6, RANGE=7)
TYPE_BY_TOKEN = {"F": 0, "U32": 1, "TIME": 2, "DATE": 3, "U8": 4, "I8": 5,
                 "H8": 6, "U16": 7, "H16": 8, "I16": 9, "B": 10, "E": 11, "S": 12}
TYPE_SIZE = {0: 4, 1: 4, 2: 4, 3: 4, 4: 1, 5: 1, 6: 1, 7: 2, 8: 2, 9: 2, 10: 1, 11: 1, 12: 0}
EPOCH_DAY = (datetime.date(1970, 1, 1) - datetime.date(1970, 1, 1)).days
SIM_DATE = datetime.date(2026, 1, 1)  # base date for TIME values


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def days_civil(y: int, m: int, d: int) -> int:
    return (datetime.date(y, m, d) - datetime.date(1970, 1, 1)).days


class Db:
    """Parameter database parsed from a captured 'l' listing."""

    def __init__(self, text: str):
        self.params = {}      # id -> dict
        self.sections = []
        self.count = 0
        cur_sec, cur_grp = "", ""
        for line in text.splitlines():
            line = line.rstrip("\r\n")
            if line.startswith("[L") and "]" in line:
                path = line.split("]", 1)[1].strip()
                parts = path.split("/")
                cur_grp = parts[-1]
                if len(parts) >= 2 and parts[1] not in self.sections:
                    self.sections.append(parts[1])
                cur_sec = parts[1] if len(parts) >= 2 else ""
                continue
            stripped = line.lstrip()
            if not stripped[:1].isdigit():
                continue
            try:
                pid_s, token, rest = stripped.split(" ", 2)
                pid = int(pid_s)
                t = TYPE_BY_TOKEN[token]
            except (ValueError, KeyError):
                continue
            if " = " not in rest:
                continue
            name, vr = rest.split(" = ", 1)
            ro = vr.rstrip().endswith("*")
            vr = vr.rstrip().rstrip("*").rstrip()
            opts, rng = None, None
            if vr.endswith("]") and " [" in vr:
                vr, r = vr.rsplit(" [", 1)
                r = r[:-1]
                if "|" in r:
                    opts = r.split("|")
                elif ".." in r:
                    lo, hi = r.split("..", 1)
                    rng = (self._num(lo), self._num(hi))
            p = dict(t=t, name=name, sec=cur_sec, grp=cur_grp, ro=ro,
                     opts=opts, rng=rng, raw=self._value(t, vr, opts))
            self.params[pid] = p
        self.count = (max(self.params) + 1) if self.params else 0

    @staticmethod
    def _num(s: str):
        s = s.replace(" ", "")
        return float(s) if ("." in s) else int(s)

    def _value(self, t, vr, opts):
        """Captured display text -> raw u32; anything unparsable (OVERFLOW
        views, free-form strings) becomes 0."""
        try:
            vr = vr.strip()
            if t in (12, 13):
                return 0
            if t in (10, 11):
                return opts.index(vr) if opts and vr in opts else 0
            if t == 0:
                return struct.unpack("<I", struct.pack("<f", float(vr.replace(" ", "").replace("+", ""))))[0]
            if t == 2:
                h, m = vr.split(":")
                return (days_civil(SIM_DATE.year, SIM_DATE.month, SIM_DATE.day) * 86400
                        + int(h) * 3600 + int(m) * 60)
            if t == 3:
                d, m, y = vr.split(".")
                return days_civil(2000 + int(y), int(m), int(d)) * 86400
            return int(vr.replace(" ", ""))
        except (ValueError, struct.error):
            return 0

    def raw_to_le(self, p, raw=None):
        raw = p["raw"] if raw is None else raw
        sz = TYPE_SIZE[p["t"]]
        return int(raw).to_bytes(sz, "little")

    def clamp(self, p, raw):
        if p["t"] in (10, 11) and p["opts"]:
            return max(0, min(raw, len(p["opts"]) - 1))
        if p["rng"]:
            lo, hi = p["rng"]
            lo = struct.unpack("<I", struct.pack("<f", lo))[0] if p["t"] == 0 else int(lo)
            hi = struct.unpack("<I", struct.pack("<f", hi))[0] if p["t"] == 0 else int(hi)
            return max(lo, min(raw, hi))
        mask = (1 << (8 * TYPE_SIZE[p["t"]])) - 1
        return raw & mask


class Sim:
    def __init__(self, port: str, listing: bytes, echo=True):
        self.db = Db(listing.decode("utf-8", "replace"))
        self.listing = listing
        self.echo = echo
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.02
        self.ser.rts = False  # do not reset the gateway board
        self.ser.dtr = False
        self.ser.open()
        self.stats = {}

    def frame(self, cmd, pid, payload=b""):
        body = bytes([cmd | 0x80, pid & 0xFF, pid >> 8, len(payload)]) + payload
        return bytes([SOF]) + body + bytes([crc8(body)])

    def handle_bin(self, cmd, pid, payload):
        self.stats[cmd] = self.stats.get(cmd, 0) + 1
        db = self.db
        if cmd == BC["PING"]:
            ver = b"SKE02/3425" + struct.pack("<H", db.count)
            return self.frame(cmd, 0, ver)
        if cmd == BC["READ"] or cmd == BC["WRITE"]:
            p = db.params.get(pid)
            if p is None:
                return self.frame(cmd, pid, b"\x01")
            if cmd == BC["WRITE"]:
                if p["ro"]:
                    return self.frame(cmd, pid, b"\x03")
                raw = int.from_bytes(payload, "little")
                p["raw"] = db.clamp(p, raw)
            return self.frame(cmd, pid, b"\x00" + bytes([p["t"]]) + db.raw_to_le(p))
        if cmd == BC["READ_MANY"]:
            out, n, rid = bytearray([0]), 0, pid
            while rid < db.count and n < 16:
                p = db.params.get(rid)
                if p is not None and TYPE_SIZE[p["t"]] > 0:
                    out += struct.pack("<H", rid) + bytes([p["t"]]) + db.raw_to_le(p)
                    n += 1
                rid += 1
            return self.frame(cmd, pid, bytes(out))
        if cmd == BC["RANGE"]:
            p = db.params.get(pid)
            if p is None:
                return self.frame(cmd, pid, b"\x01")
            if p["t"] in (10, 11) and p["opts"]:
                out = bytearray([0, p["t"], len(p["opts"])])
                for o in p["opts"]:
                    b = o.encode("utf-8")
                    out.append(len(b))
                    out += b
                return self.frame(cmd, pid, bytes(out))
            if p["rng"]:
                sz = TYPE_SIZE[p["t"]]
                lo, hi = p["rng"]
                if p["t"] == 0:
                    lo = struct.unpack("<I", struct.pack("<f", lo))[0]
                    hi = struct.unpack("<I", struct.pack("<f", hi))[0]
                return self.frame(cmd, pid,
                                  b"\x00" + bytes([p["t"]]) +
                                  int(lo).to_bytes(sz, "little") +
                                  int(hi).to_bytes(sz, "little"))
            return self.frame(cmd, pid, b"\x00" + bytes([p["t"]]))
        if cmd == BC["MENU"]:
            p = db.params.get(pid)
            if p is None:
                return self.frame(cmd, pid, b"\x01")
            parts = ["Hacтpoйки", p["sec"]] + ([p["grp"]] if p["grp"] != p["sec"] else [])
            path = "/".join(parts).encode("utf-8")
            return self.frame(cmd, pid,
                              b"\x00" + bytes([len(parts) - 1, len(path)]) + path)
        if cmd == BC["REBOOT"]:
            print("sim: REBOOT requested", file=sys.stderr)
            return self.frame(cmd, 0, b"\x00")
        return self.frame(cmd, pid, b"\x02")

    def handle_text(self, line):
        line = line.strip()
        if line == "l":
            self.ser.write(self.listing)
        elif line == "?":
            self.ser.write(b"sim console: l\n")
        elif line[:1] == "i":
            self.ser.write(f"model SKE02 fw 3425 params {self.db.count}\r\n".encode())
        # 'g'/'s'/'d' not needed by the gateway

    def run(self):
        print(f"sim: {self.db.count} params, {len(self.db.sections)} sections: "
              f"{self.db.sections}", file=sys.stderr)
        buf, mode, need = bytearray(), None, 0
        while True:
            data = self.ser.read(256)
            if not data:
                continue
            if self.echo:
                self.ser.write(data)  # the real console echoes its input
            for b in data:
                if mode is None:
                    mode = "bin" if b == SOF else "txt"
                    buf.clear()
                    if mode == "txt":
                        buf.append(b)
                        continue
                buf.append(b)
                if mode == "txt":
                    if b in (13, 10):
                        line = bytes(buf[:-1]).decode("utf-8", "replace")
                        buf.clear()
                        mode = None
                        if line.strip():
                            self.handle_text(line)
                elif len(buf) >= 5:
                    ln = buf[4]
                    if len(buf) == 5 + ln + 1:
                        if crc8(bytes(buf[:-1])) == buf[-1]:
                            cmd, pid = buf[0], buf[1] | (buf[2] << 8)
                            self.ser.write(self.handle_bin(cmd, pid, bytes(buf[5:5 + ln])))
                        buf.clear()
                        mode = None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--listing", default=None)
    ap.add_argument("--no-echo", action="store_true")
    args = ap.parse_args()

    listing = None
    for cand in (args.listing, "C:/tmp/ske_l.txt",
                 __file__.rsplit("/", 1)[0] + "/ske_l.txt"):
        if cand:
            try:
                listing = open(cand, "rb").read()
                break
            except OSError:
                continue
    if listing is None:
        sys.exit("listing not found; capture one first: see README, tools/")

    sim = Sim(args.port, listing, echo=not args.no_echo)
    try:
        sim.run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
