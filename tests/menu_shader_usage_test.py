"""Tests tools/menu_shader_usage.py with synthetic shader-usage lines."""

import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import menu_shader_usage as msu  # noqa: E402

LOGLINES = [
    '[shader-usage] {"stage":"ps","sha256":"' + "a" * 64 + '","bytes":200,'
    '"caller":"0x00f883df","result":"0x00000000","plan":null,"plan_hit":0}',
    '[shader-usage] {"stage":"ps","sha256":"' + "a" * 64 + '","bytes":200,'
    '"caller":"0x00f883df","result":"0x00000000","plan":null,"plan_hit":0}',
    '[shader-usage] {"stage":"vs","sha256":"' + "b" * 64 + '","bytes":172,'
    '"caller":"0x00f8838c","result":"0x00000000","plan":null,"plan_hit":0}',
    '[shader-usage] {"stage":"ps","sha256":"' + "c" * 64 + '","bytes":332,'
    '"caller":"0x00ccbbc5","result":"0x00000000","plan":null,"plan_hit":0}',
    '[shader-usage] {"stage":"ps","sha256":"' + "d" * 64 + '","bytes":184,'
    '"caller":"0x00f883df","result":"0x00000000","plan":"menu.composite.ps",'
    '"plan_hit":1}',
]


def main():
    checks = 0
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "usage.log"
        log.write_text("\n".join(LOGLINES) + "\n", encoding="utf-8")
        plans = Path(tmp) / "plans.json"
        plans.write_text(json.dumps({"entries": [{
            "id": "menu.composite.ps", "stage": "ps_3_0", "sha256": "d" * 64,
            "draw_enabled": 1, "family": "generic",
        }]}), encoding="utf-8")
        out = Path(tmp) / "out.json"
        assert msu.main([str(log), "--json", str(out), "--plans", str(plans)]) == 0
        data = json.loads(out.read_text(encoding="utf-8"))
        assert data["lines"] == 5
        assert data["unique"] and len(data["unique"]) == 4
        checks += 1
        by = {r["sha256"]: r for r in data["unique"]}
        assert by["a" * 64]["calls"] == 2 and by["a" * 64]["class"] == "gfx"
        assert by["b" * 64]["stage"] == "vs"
        assert by["c" * 64]["class"] == "shaderfx_preload"
        assert by["d" * 64]["in_shader_plans"] is True
        assert by["d" * 64]["plan_draw_enabled"] == 1
        assert by["d" * 64]["plan_hit"] == 1
        checks += 1
        assert msu.classify("0x00f883df") == "gfx"
        assert msu.classify("0x00ccbbc5") == "shaderfx_preload"
        assert msu.classify("nonsense") == "unknown"
        checks += 1
    print("menu_shader_usage_test: %d checks OK" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
