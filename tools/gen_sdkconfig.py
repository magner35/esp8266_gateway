#!/usr/bin/env python3
"""Generate the project sdkconfig.<env> without interactive menuconfig.

PlatformIO's esp8266-rtos-sdk builder requires sdkconfig.<env> to already
exist, while the ESP8266_RTOS_SDK v3.4 Cmake glue normally creates it from
Kconfig defaults itself. This tool replicates that step: it collects the
component Kconfig lists (what tools/cmake/kconfig.cmake does inside IDF)
and runs the SDK confgen to write the config from sdkconfig.defaults.

    python tools/gen_sdkconfig.py <env-name>
"""

import json
import os
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
SDK = Path(os.path.expanduser("~/.platformio/packages/framework-esp8266-rtos-sdk"))


def main() -> None:
    env_name = sys.argv[1] if len(sys.argv) > 1 else "esp8266_modern_rtos"
    sdkconfig = PROJECT / f"sdkconfig.{env_name}"

    kconfigs = sorted(str(p).replace("\\", "/") for p in SDK.glob("components/*/Kconfig"))
    projbuilds = sorted(
        str(p).replace("\\", "/") for p in SDK.glob("components/*/Kconfig.projbuild")
    )
    env = {
        "IDF_PATH": str(SDK).replace("\\", "/"),
        "IDF_TARGET": "esp8266",
        "IDF_CMAKE": "y",
        "COMPONENT_KCONFIGS": " ".join(kconfigs),
        "COMPONENT_KCONFIGS_PROJBUILD": " ".join(projbuilds),
    }
    env_file = PROJECT / "tools" / "kconfig.env.json"
    env_file.write_text(json.dumps(env), encoding="utf-8")

    cmd = [
        sys.executable,
        str(SDK / "tools" / "kconfig_new" / "confgen.py"),
        "--kconfig", str(SDK / "Kconfig"),
        "--env-file", str(env_file),
        "--defaults", str(PROJECT / "sdkconfig.defaults"),
        "--config", str(sdkconfig),
        "--output", "config", str(sdkconfig),
    ]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)
    print(f"written: {sdkconfig}")


if __name__ == "__main__":
    main()
