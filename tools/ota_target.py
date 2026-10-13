"""Цель `pio run -t ota`: копирует образ приложения в корень проекта
под понятным именем gw_update.bin - именно его заливают через
"Обновление ПО шлюза" (bootloader/partitions для OTA не нужны)."""
import shutil

Import("env")


def make_ota(source, target, env):
    src = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    dst = env.subst("$PROJECT_DIR/gw_update.bin")
    shutil.copyfile(src, dst)
    print("OTA image ready: %s (upload it in Сервис -> Обновление ПО шлюза)" % dst)


env.AddCustomTarget(
    "ota",
    "$BUILD_DIR/${PROGNAME}.bin",
    make_ota,
    title="copy firmware.bin -> gw_update.bin",
    always_build=True,
)
