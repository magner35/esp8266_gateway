#!/usr/bin/env python3
"""Снять свежий листинг с прибора на COM28 (LIST 1/2/3) в файл."""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM28"
OUT = sys.argv[2] if len(sys.argv) > 2 else "tools/ske_l2.txt"

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.05
s.rts = False   # RTS на RESET у загрузчика прибора
s.dtr = False
s.open()
time.sleep(0.3)
s.reset_input_buffer()

buf = bytearray()


def drain(sec):
    end = time.time() + sec
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf.extend(d)
            end = time.time() + 0.6  # продлеваем, пока идёт вывод


s.write(b"ECHO\n")
drain(1.5)
buf.clear()
for cmd in (b"LIST 1", b"LIST 2", b"LIST 3"):
    s.write(cmd + b"\n")
    drain(12)
    time.sleep(0.3)

with open(OUT, "wb") as f:
    f.write(bytes(buf))
print(f"captured {len(buf)} bytes -> {OUT}")
s.close()
