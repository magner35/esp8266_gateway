"""Pre-build: touch main.c чтобы __DATE__/__TIME__ обновились при каждой
сборке. Без этого PIO не перекомпилирует main.c (инкрементальная сборка)
и штамп версии остаётся от прошлого релиза."""

import os
import pathlib

Import("env")  # noqa: F821

ROOT = pathlib.Path(env.get("PROJECT_DIR"))  # noqa: F821
main_c = ROOT / "src/main.c"
if main_c.exists():
    os.utime(str(main_c), None)
