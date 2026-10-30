#!/usr/bin/env python3
"""PlatformIO post-build hook: собрал прошивку -> собери пакеты .skpak.

Подключён в platformio.ini:
    extra_scripts = post:tools/pack_after_build.py

После каждой удачной сборки шлюза обновляет:
  dist/system_full.skpak  - ЕДИНЫЙ образ (esp + avr) + versions.txt
                            для сервера обновлений (консоль: DOWNLOAD)
  dist/curl/gateway.skpak - отдельные пакеты для заливки через curl
  dist/curl/meter.skpak     (веб-эндпоинты /api/ota/upload и
                            /api/avr/upload), без versions.txt

Молча пропускает упаковку, если сборки прибора нет; если нет
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
CURL = ROOT / "dist/curl"


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
    """версия шлюза из src/version.h (GW_FWVERSION), единообразно
    с прибором; fallback — дата из маркера в образе"""
    ver_h = ROOT / "src/version.h"
    try:
        m = re.search(r'#define\s+GW_FWVERSION\s+"([^"]+)"',
                      ver_h.read_text(encoding="utf-8", errors="replace"))
        if m:
            return m.group(1)
    except OSError:
        pass
    # fallback: дата из маркера
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
    bundle = DIST / "system_full.skpak"
    if not bundle.exists():
        return
    from datetime import date
    today = date.today().isoformat()
    line = (f"bundle {_gw_version() or 'dev'}+{_meter_version() or 'dev'} "
            f"{today} {bundle.name} {bundle.stat().st_size} gw+meter")
    (DIST / "versions.txt").write_text(line + "\n", encoding="utf-8")
    print("pack: versions.txt (bundle only)")


def package(source, target, env):
    binpath = BUILD / "firmware.bin"
    if not binpath.exists():
        print("pack: no firmware.bin, skip")
        return
    if not (ROOT / "tools/keys").exists():
        print("pack: no tools/keys - run 'python tools/ota_pack.py genkeys'")
        return
    mhex = METER / ".pio/build/ATmega128/firmware.hex"
    if not mhex.exists():
        print("pack: no meter build, skip packing")
        return
    DIST.mkdir(exist_ok=True)
    # отдельные пакеты больше не живут в корне dist - убрать застарелые
    for stale in ("gateway.skpak", "meter.skpak"):
        p = DIST / stale
        if p.exists():
            p.unlink()
            print(f"pack: removed stale {stale}")
    args = ["pack-all", str(binpath), str(mhex),
            str(DIST / "system_full.skpak")]
    mver = _meter_version()
    if mver:
        args += ["--mver", mver]
    _run(args)
    # отдельные пакеты для curl-заливки (без versions.txt)
    CURL.mkdir(exist_ok=True)
    _run(["pack", str(binpath), "esp", str(CURL / "gateway.skpak"),
          "--note", "gw"])
    _run(["pack", str(mhex), "avr", str(CURL / "meter.skpak"),
          "--note", "meter"])
    print("pack: dist/curl/ gateway.skpak + meter.skpak")
    _write_versions()


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", package)
