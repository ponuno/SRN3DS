"""Add RT_PROF hooks to an already block-instrumented generated directory."""
import importlib.util
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location("ig", "tools/instrument_generated.py")
ig = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ig)

root = Path(sys.argv[1])
total = 0
for path in sorted(root.glob("*_[0-9][0-9].c")):
    text = path.read_text(encoding="utf-8")
    if "n3ds_pilot_tick" not in text:
        continue
    text, count = ig.add_profile_hooks(text)
    if count:
        if '#include "rt_prof.h"' not in text:
            text = '#include "rt_prof.h"\n' + text
        path.write_text(text, encoding="utf-8")
        total += count
print(f"profiled {total} functions in {root}")
