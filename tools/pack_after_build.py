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


def _gw_version():
    """версия шлюза: дата сборки из маркера в образе (SKEGWv1:...)"""
    binpath = BUILD / "firmware.bin"
    try:
        data = binpath.read_bytes()
        off = data.find(b"SKEGWv1:")
        if off >= 0:
            v = data[off + 8:off + 40].split(bytes([0]))[0]
            return v.decode("ascii", "replace").strip()
    except OSError:
        pass
    return ""


def _write_versions():
    """versions.txt для локального тест-сервера (python -m http.server)"""
    lines = []
    gw = DIST / "gateway.skpak"
    meter = DIST / "meter.skpak"
    bundle = DIST / "system_full.skpak"
    gwver = _gw_version()
    mver = _meter_version()
    from datetime import date
    today = date.today().isoformat()
    if gw.exists():
        lines.append(f"gw {gwver or 'dev'} {today} {gw.name} "
                     f"{gw.stat().st_size} gateway firmware")
    if meter.exists():
        lines.append(f"meter {mver or 'dev'} {today} {meter.name} "
                     f"{meter.stat().st_size} meter firmware")
    if bundle.exists():
        lines.append(f"bundle {mver or 'dev'} {today} {bundle.name} "
                     f"{bundle.stat().st_size} unified gw+meter")
    if lines:
        (DIST / "versions.txt").write_text("\n".join(lines) + "\n",
                                           encoding="utf-8")
        print(f"pack: versions.txt ({len(lines)} entries)")


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
    _write_versions()


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", package)
