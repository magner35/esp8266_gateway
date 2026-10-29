#!/usr/bin/env python3
"""PlatformIO post-build hook: собрал прошивку -> собери пакеты .skpak.

Подключён в platformio.ini:
    extra_scripts = post:tools/pack_after_build.py

После каждой удачной сборки шлюза обновляет dist/:
  gateway.skpak      - образ ESP (только что собранный)
  meter.skpak        - hex прибора, если ../Totalizer_FreeRTOS сборка есть
  system_full.skpak  - ЕДИНЫЙ образ (esp + avr; ПРИНИМАЕТСЯ прошивкой
                       с bundle-поддержкой - пока просто хранится)

Молча пропускает meter-часть, если сборки прибора нет; если нет
ключей tools/keys/ - печатает подсказку и не ломает сборку.
"""
import re

Import("env")  # noqa: F821  (SCons provides it)

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(env.get("PROJECT_DIR"))  # noqa: F821
BUILD = ROOT / ".pio/build/esp8266_modern_rtos"
METER = ROOT.parent / "Totalizer_FreeRTOS"
PACK = ROOT / "tools/ota_pack.py"
DIST = ROOT / "dist"


def _run(args):
    subprocess.run([sys.executable, str(PACK)] + args,
                   cwd=str(ROOT), check=False)


def _meter_version():
    verh = METER / "include/board.h"
    try:
        m = re.search(r'#define\s+FWVERSION\s+"([^"]+)"',
                      verh.read_text(encoding="utf-8", errors="replace"))
        return m.group(1) if m else ""
    except OSError:
        return ""


def package(source, target, env):
    binpath = BUILD / "firmware.bin"
    if not binpath.exists():
        print("pack: no firmware.bin, skip")
        return
    if not (ROOT / "tools/keys").exists():
        print("pack: no tools/keys - run 'python tools/ota_pack.py genkeys'")
        return
    DIST.mkdir(exist_ok=True)
    _run(["pack", str(binpath), "esp", str(DIST / "gateway.skpak"),
          "--note", "gw"])
    mhex = METER / ".pio/build/ATmega128/firmware.hex"
    if mhex.exists():
        mver = _meter_version()
        _run(["pack", str(mhex), "avr", str(DIST / "meter.skpak"),
              "--note", "meter"])
        args = ["pack-all", str(binpath), str(mhex),
                str(DIST / "system_full.skpak")]
        if mver:
            args += ["--mver", mver]
        _run(args)
    else:
        print("pack: no meter build, skip meter/system packages")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", package)
