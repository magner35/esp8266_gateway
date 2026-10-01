#!/usr/bin/env python3
"""Discovery pipeline check against a real SKE-02 console.

Replicates, step by step, what the gateway firmware does (see
src/ske02.cpp): PING, the text 'l' listing (names/sections/groups/
read-only), the READ_MANY sweep and the per-parameter RANGE queries -
including tolerance to the console echo. Use it to validate protocol
assumptions without the ESP hardware.

    python tools/discovery_check.py COM28
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("нужен pyserial: pip install pyserial")

SOF = 0xAA
CMD_PING, CMD_READ, CMD_WRITE, CMD_READ_MANY, CMD_REBOOT, CMD_MENU, CMD_RANGE = range(1, 8)
TYPE_SIZE = {0: 4, 1: 4, 2: 4, 3: 4, 4: 1, 5: 1, 6: 1, 7: 2, 8: 2, 9: 2, 10: 1, 11: 1}
TYPENAME = ["F", "U32", "TIME", "DATE", "U8", "I8", "H8", "U16", "H16", "I16", "B", "E"]


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


class Console:
    def __init__(self, port: str):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.35
        self.ser.rts = False  # the meter routes RTS to RESET
        self.ser.dtr = False
        self.ser.open()

    def transact(self, cmd: int, pid: int, payload: bytes = b"") -> bytes:
        self.ser.reset_input_buffer()
        body = bytes([cmd, pid & 0xFF, pid >> 8, len(payload)]) + payload
        self.ser.write(bytes([SOF]) + body + bytes([crc8(body)]))
        return self.read_frame(cmd, pid, deadline=time.monotonic() + 0.5)

    def read_frame(self, cmd: int, pid: int, deadline: float) -> bytes:
        """Same resync rules as the firmware: skip the echoed request
        (its cmd lacks the 0x80 bit) and stray bytes before SOF."""
        buf = bytearray()

        def rx(timeout_left):
            self.ser.timeout = max(0.01, min(0.35, timeout_left))
            b = self.ser.read(1)
            return b[0] if b else None

        while time.monotonic() < deadline:
            c = rx(deadline - time.monotonic())
            if c is None:
                raise IOError("timeout waiting SOF")
            if c != SOF:
                continue
            hdr = bytearray()
            while len(hdr) < 4:
                c = rx(deadline - time.monotonic())
                if c is None:
                    raise IOError("timeout in header")
                hdr.append(c)
            if hdr[0] != (cmd | 0x80) or (hdr[1] | (hdr[2] << 8)) != pid:
                continue  # echo or noise: keep scanning
            ln = hdr[3]
            while len(buf) < ln:
                c = rx(deadline - time.monotonic())
                if c is None:
                    raise IOError("timeout in body")
                buf.append(c)
            c = rx(deadline - time.monotonic())
            if c is None or crc8(bytes(hdr) + bytes(buf)) != c:
                continue  # corrupted: resync
            return bytes(buf)
        raise IOError("deadline")

    def ping(self):
        self.ser.write(b"\r")  # drop a half-typed text line, like the gateway
        time.sleep(0.05)
        body = self.transact(CMD_PING, 0)
        ver, cnt = body[:-2], struct.unpack("<H", body[-2:])[0]
        return ver.decode("utf-8", "replace"), cnt

    def text_list(self):
        self.ser.reset_input_buffer()
        self.ser.write(b"l\r\n")
        end = time.monotonic() + 12
        quiet = time.monotonic() + 0.7
        buf = bytearray()
        while time.monotonic() < end:
            chunk = self.ser.read(512)
            if chunk:
                buf += chunk
                quiet = time.monotonic() + 0.7
            elif time.monotonic() > quiet:
                break
        return buf.decode("utf-8", "replace")

    def read_many(self, start: int):
        body = self.transact(CMD_READ_MANY, start, bytes([16]))
        if not body or body[0] != 0:
            raise IOError(f"READ_MANY status {body[:1].hex()}")
        i, recs, last = 1, [], start
        while i + 3 <= len(body):
            rid = body[i] | (body[i + 1] << 8)
            t = body[i + 2]
            sz = TYPE_SIZE.get(t, 0)
            if i + 3 + sz > len(body):
                break
            recs.append((rid, t, body[i + 3:i + 3 + sz]))
            last = rid
            i += 3 + sz
        return recs, last

    def query_range(self, pid: int):
        """-> ("num", min, max) | ("opts", [..]) | ("none",)"""
        body = self.transact(CMD_RANGE, pid)
        if not body or body[0] != 0:
            raise IOError(f"RANGE status {body[:1].hex()}")
        t = body[1]
        if t in (10, 11):  # option list first: its length also satisfies
            if len(body) < 3:  # the numeric size check
                return "none",
            cnt, i, opts = body[2], 3, []
            for _ in range(cnt):
                if i >= len(body):
                    break
                ln = body[i]
                opts.append(body[i + 1:i + 1 + ln].decode("utf-8", "replace"))
                i += 1 + ln
            return "opts", opts
        sz = TYPE_SIZE.get(t, 0)
        if sz and len(body) >= 2 + 2 * sz:
            lo = int.from_bytes(body[2:2 + sz], "little")
            hi = int.from_bytes(body[2 + sz:2 + 2 * sz], "little")
            return "num", lo, hi
        return "none",


def parse_listing(text: str):
    """Port of parseTextLine(): sections from [L1] headers, groups from
    the last path component of any header, names and '*' read-only."""
    params, sections, groups = {}, [], []
    cur_sec, cur_grp = -1, ""

    def add(pool, name):
        if name in pool:
            return pool.index(name)
        pool.append(name)
        return len(pool) - 1

    for line in text.splitlines():
        line = line.rstrip("\r\n")
        if not line:
            continue
        if line.startswith("[L") and "]" in line:
            path = line.split("]", 1)[1].strip()
            last = path.split("/")[-1]
            cur_grp = last
            parts = path.split("/")
            if len(parts) >= 2:
                cur_sec = add(sections, parts[1])
            continue
        line = line.lstrip()  # param lines are indented 2 spaces per level
        if not line or not line[0].isdigit():
            continue  # echo/noise
        try:
            head, rest = line.lstrip().split(" ", 1)
            pid = int(head)
            toks = rest.split(" ", 1)
            nameval = toks[1] if len(toks) > 1 else ""
            if " = " not in nameval:
                continue
            name, _val = nameval.split(" = ", 1)
        except ValueError:
            continue
        ro = line.rstrip().endswith("*")
        params[pid] = {"name": name, "ro": ro, "sec": cur_sec, "grp": cur_grp}
    return params, sections, groups


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    args = ap.parse_args()
    con = Console(args.port)

    ver, cnt = con.ping()
    print(f"PING ok: {ver}, {cnt} params")

    params, sections, groups = parse_listing(con.text_list())
    print(f"'l' parsed: {len(params)} named params, sections={sections}")
    if len(sections) != 4:
        print("!! expected 4 first-level sections")

    values = {}
    cursor, chunks = 0, 0
    while cursor < cnt:
        recs, last = con.read_many(cursor)
        chunks += 1
        for rid, t, raw in recs:
            values[rid] = (t, raw)
        cursor = last + 1
    print(f"sweep done: {len(values)} values in {chunks} chunks")

    def fmt(t: int, v: int):
        if t == 0:
            return round(struct.unpack("<f", struct.pack("<I", v))[0], 4)
        if t == 5:
            return struct.unpack("<b", struct.pack("<B", v & 0xFF))[0]
        if t == 9:
            return struct.unpack("<h", struct.pack("<H", v & 0xFFFF))[0]
        return v

    ranges, opts, misses = 0, 0, 0
    for pid in sorted(values):
        t, raw = values[pid]
        if t in (12, 13):
            continue
        try:
            r = con.query_range(pid)
        except IOError:
            misses += 1
            continue
        if r[0] == "num":
            ranges += 1
            if pid in (2, 43, 46, 80, 134, 154):
                print(f"  range {pid:3d} {TYPENAME[t]:4}: {fmt(t, r[1])}..{fmt(t, r[2])}"
                      f"  (value {fmt(t, int.from_bytes(raw, 'little'))})")
        elif r[0] == "opts":
            opts += 1
            if pid in (0, 1, 44, 49):
                print(f"  opts  {pid:3d} {TYPENAME[t]:4}: {r[1]}")
    print(f"RANGE done: {ranges} numeric, {opts} option lists, {misses} failed")

    # sanity per section
    for si, s in enumerate(sections):
        n = sum(1 for p in params.values() if p["sec"] == si)
        print(f"  section '{s}': {n} named params")
    nosec = sum(1 for p in params.values() if p["sec"] < 0)
    if nosec:
        print(f"  !! {nosec} params without a section")


if __name__ == "__main__":
    main()
