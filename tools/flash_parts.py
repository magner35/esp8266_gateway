"""
Auto-flash the partition table after every `pio run -t upload`.

PlatformIO's esp8266-rtos-sdk builder does NOT flash partitions.bin
(no write_flash in builder/frameworks/esp8266-rtos-sdk.py) - only
boot+app. With a custom partitions.csv (nvs 64K) this left the device
running the stock 24K table forever. This post-action writes
partitions.bin to 0x8000 using the SAME esptool/port/upload-speed
PlatformIO uses for the app upload. Idempotent: re-flashing an
identical table is harmless.
"""

Import("env")

import os
import subprocess
import sys


def _after_upload(source, target, env):
    try:
        platform = env.PioPlatform()
        pkg = platform.get_package("tool-esptoolpy")
        esptool = os.path.join(pkg.path, "esptool.py") if pkg else "esptool.py"
    except Exception:
        esptool = "esptool.py"

    port = env.subst("$UPLOAD_PORT")
    speed = env.subst("$UPLOAD_SPEED") or "921600"
    parts = os.path.join(env.subst("$BUILD_DIR"), "partitions.bin")

    if not os.path.isfile(parts):
        print("!! flash_parts: %s not found (build first)" % parts)
        return

    cmd = [sys.executable, esptool,
           "--port", port, "--baud", str(speed),
           "write_flash", "0x8000", parts]
    print("== flash_parts: writing partition table ==")
    print("   " + " ".join(cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        print("!! flash_parts: FAILED rc=%d" % rc)
    else:
        print("== flash_parts: partition table flashed OK ==")


env.AddPostAction("upload", _after_upload)
