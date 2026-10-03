"""PlatformIO pre-build hook: regenerate src/pages.h from www/*.html so
HTML/JS edits are picked up by a plain `pio run` - no manual step."""
Import("env")  # noqa: F401  (SCons builtin)

import pathlib
import sys

HERE = pathlib.Path(env.subst("$PROJECT_DIR")) / "tools"
sys.path.insert(0, str(HERE))
import pages  # noqa: E402

pages.build()
