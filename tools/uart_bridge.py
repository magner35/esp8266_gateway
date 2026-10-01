#!/usr/bin/env python3
"""Software UART bridge: gateway ESP8266 <-> SKE-02 service console.

For bench testing without wiring the boards together: the ESP8266 USB
port and the meter console USB port are cross-connected through the PC.

    ESP TX (COM11 RX) -> bridge -> COM28 TX -> meter RX
    meter TX (COM28 RX) -> bridge -> COM11 TX -> ESP RX

Usage:
    python tools/uart_bridge.py COM11 COM28 [--esp-reset]

The meter port is always opened with RTS/DTR deasserted: its bootloader
wiring routes RTS to RESET. For the ESP side the same is done unless
--esp-reset is given (opening the port then power-cycles the ESP, which
is handy to watch a fresh discovery cycle).
"""

import argparse
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("нужен pyserial: pip install pyserial")


def open_port(name: str, reset_on_open: bool) -> serial.Serial:
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.timeout = 0.05
    s.rts = reset_on_open
    s.dtr = reset_on_open
    s.open()
    return s


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("gw")      # ESP8266 side, e.g. COM11
    ap.add_argument("meter")   # SKE-02 side, e.g. COM28
    ap.add_argument("--esp-reset", action="store_true")
    args = ap.parse_args()

    gw = open_port(args.gw, args.esp_reset)
    meter = open_port(args.meter, False)
    print(f"bridge {args.gw} <-> {args.meter} @115200, Ctrl-C to stop")

    stop = threading.Event()

    def pump(src: serial.Serial, dst: serial.Serial, tag: str) -> None:
        try:
            while not stop.is_set():
                data = src.read(256)
                if data:
                    dst.write(data)
        except Exception as e:  # port closed etc.
            print(f"pump {tag} stopped: {e}", file=sys.stderr)
            stop.set()

    t1 = threading.Thread(target=pump, args=(gw, meter, "gw->meter"), daemon=True)
    t2 = threading.Thread(target=pump, args=(meter, gw, "meter->gw"), daemon=True)
    t1.start()
    t2.start()

    try:
        while not stop.wait(1):
            pass
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        time.sleep(0.1)
        gw.close()
        meter.close()
        print("bridge closed")


if __name__ == "__main__":
    main()
