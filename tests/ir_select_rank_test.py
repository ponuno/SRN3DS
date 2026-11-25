"""Synthetic ranking tests for tools/ir_select.py.

Proves time ranking differs from call ranking, weighted_insns is labelled as a
proxy, ticks/headerless time columns work, and missing time is explicit.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "ir_select.py"

FUNCTIONS = {
    "0x1000": [[0x1000, 0x100A]],   # 10 bytes
    "0x2000": [[0x2000, 0x200A]],   # 10 bytes
    "0x3000": [[0x3000, 0x3014]],   # 20 bytes
}


def run_case(base, profile_text, *extra):
    case = base / profile_text.splitlines()[0].replace(",", "_").replace(" ", "_")
    case.mkdir(parents=True, exist_ok=True)
    profile = case / "profile.csv"
    profile.write_text(profile_text, encoding="utf-8")
    functions = case / "functions.json"
    functions.write_text(json.dumps({"functions": FUNCTIONS}), encoding="utf-8")
    out = case / "out"
    cmd = [sys.executable, str(TOOL), str(profile), str(functions), "--out", str(out),
           "--exe", str(case / "missing.exe"), "--insn-per-byte", "1",
           "--emit-ir-table", str(out / "ir_table.c"), "--validate", *extra]
    p = subprocess.run(cmd, capture_output=True, text=True)
    manifest = out / "manifest.json"
    return p, json.loads(manifest.read_text(encoding="utf-8")) if manifest.exists() else None, out


def addrs(out):
    return {int(l, 16) for l in (out / "aot.txt").read_text(encoding="utf-8").splitlines() if l}


def main():
    with tempfile.TemporaryDirectory(prefix="ir_rank_") as td:
        base = Path(td)
        profile = "addr,hits,time_ns\n0x1000,1,100\n0x2000,10,10\n0x3000,5,50\n"

        p, mt, ot = run_case(base, profile, "--rank-by", "time", "--limit", "2")
        assert p.returncode == 0, p.stdout + p.stderr
        assert addrs(ot) == {0x1000, 0x3000}, addrs(ot)
        assert mt["ranking_mode"] == "time"
        assert mt["profile_time_data"] == "complete"
        assert mt["total_measured_time"] == 160
        assert mt["covered_time_share"] == 0.9375
        assert mt["aot_ir_time_split"]["aot"] == 150
        assert mt["aot_ir_time_split"]["ir"] == 10
        assert mt["aot_ir_time_split"]["basis"] == "measured_time_ns"
        assert mt["hardware_time_claim"] is False
        assert {r["addr"]: r["measured_time"] for r in mt["selected"]} == {0x1000: 100, 0x3000: 50}
        assert "partition: OK" in p.stdout
        print("  ok time ranking differs from calls and records measured split")

        p, mc, oc = run_case(base, profile, "--rank-by", "calls", "--limit", "2")
        assert p.returncode == 0, p.stdout + p.stderr
        assert addrs(oc) == {0x2000, 0x3000}, addrs(oc)
        assert mc["ranking_mode"] == "calls"
        assert mc["covered_time_share"] == 0.375
        assert mc["aot_ir_time_split"]["aot"] == 60
        assert mc["aot_ir_time_split"]["ir"] == 100
        print("  ok call ranking remains available with time shares")

        p, mw, ow = run_case(base, profile, "--rank-by", "weighted_insns", "--limit", "1")
        assert p.returncode == 0, p.stdout + p.stderr
        assert addrs(ow) == {0x2000}, addrs(ow)  # 100 ties C; address breaks tie
        assert mw["ranking_mode"] == "weighted_insns"
        assert mw["ranking_basis"] == "proxy_hits_x_insns"
        assert mw["weighted_insns_is_proxy"] is True
        assert mw["hardware_time_claim"] is False
        assert mw["selected"][0]["weighted_insns"] == 100
        assert mw["selected"][0]["estimated_time"] is None
        assert "proxy" in mw["selected"][0]["estimate_basis"].lower()
        assert "PROXY" in p.stdout
        print("  ok weighted_insns is explicitly a proxy")

        p, mk, ok = run_case(base, "addr,hits,ticks\n0x1000,1,40\n0x2000,10,4\n0x3000,5,20\n")
        assert p.returncode == 0, p.stdout + p.stderr
        assert mk["ranking_mode"] == "time"  # complete time column is the default
        assert mk["profile_time_unit"] == "ticks"
        assert mk["profile_time_unit_source"] == "header"
        assert mk["selected"][0]["ticks"] == 40
        assert mk["selected"][0]["time_ns"] is None
        print("  ok ticks column and automatic time default")

        p, mh, oh = run_case(base, "0x1000,1,100\n0x2000,10,10\n0x3000,5,50\n", "--limit", "1")
        assert p.returncode == 0, p.stdout + p.stderr
        assert mh["ranking_mode"] == "time"
        assert mh["profile_time_unit"] == "time_ns"
        assert mh["profile_time_unit_source"] == "inferred_headerless"
        print("  ok headerless third column is inferred and recorded")

        p, _, _ = run_case(base, "addr,hits\n0x1000,1\n0x2000,10\n", "--rank-by", "time")
        assert p.returncode != 0
        assert "complete time_ns/ticks" in (p.stdout + p.stderr)
        print("  ok missing time fails explicitly for --rank-by time")

        p, _, _ = run_case(base, "addr,hits,time_ns\n0x1000,1,100\n0x2000,10,\n", "--rank-by", "time")
        assert p.returncode != 0
        assert "missing at 00002000" in (p.stdout + p.stderr)
        print("  ok partial time fails explicitly")

        p, mn, on = run_case(base, "addr,hits\n0x1000,1\n0x2000,10\n", "--limit", "1")
        assert p.returncode == 0, p.stdout + p.stderr
        assert mn["ranking_mode"] == "calls"
        assert mn["profile_time_data"] == "missing"
        assert mn["covered_time_share"] is None
        assert mn["aot_ir_time_split"]["basis"] == "unavailable"
        assert "no measured time share is claimed" in p.stdout
        print("  ok addr,hits stays backward compatible with explicit null time")

    print("IR selection ranking tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
