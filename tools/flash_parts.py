"""
Auto-flash the partition table + initial otadata after every
`pio run -t upload`.

PlatformIO's esp8266-rtos-sdk builder does NOT flash partitions.bin
(no write_flash in builder/frameworks/esp8266-rtos-sdk.py) - only
boot+app. With a custom partitions.csv (nvs 64K, two OTA slots) this
left the device running the stock table forever.

otadata (0x1A000) flashes the INITIAL image: it makes the bootloader
and esp_ota_get_running_partition() AGREE on ota_0 after a wired
upload. Without it a stale otadata (from a previous OTA) can point to
the OTHER slot than the one pio just wrote - and the next OTA would
erase the slot the device actually boots from: endless
"Fatal exception (0) call_start_cpu" boot loop.
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
    bld = env.subst("$BUILD_DIR")
    parts = os.path.join(bld, "partitions.bin")

    if not os.path.isfile(parts):
        print("!! flash_parts: %s not found (build first)" % parts)
        return

    cmd = [sys.executable, esptool,
           "--port", port, "--baud", str(speed),
           "write_flash", "0x8000", parts]
    otadata = os.path.join(bld, "ota_data_initial.bin")
    if os.path.isfile(otadata):
        cmd += ["0x1A000", otadata]

    print("== flash_parts: writing partition table + otadata ==")
    print("   " + " ".join(cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        print("!! flash_parts: FAILED rc=%d" % rc)
    else:
        print("== flash_parts: partition table + otadata flashed OK ==")


env.AddPostAction("upload", _after_upload)
