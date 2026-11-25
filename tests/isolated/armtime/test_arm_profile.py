"""Host-side tests for tools/arm_profile.py; no Azahar required."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / "tools" / "arm_profile.py"
FIXTURE = ROOT / "tests" / "isolated" / "armtime" / "fixture.json"


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)],
                          capture_output=True, text=True)


def make_run(base):
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    functions = []
    csv_lines = ["addr,hits,ticks"]
    for i, item in enumerate(fixture["functions"], 1):
        aot = i * i * 100
        warm = aot * 3 + 17
        cold = warm + 500 * fixture["cold_reps"]
        functions.append({
            "addr": item["addr"], "end": item["end"], "name": item["name"],
            "workload": item["workload"], "hits": item["hits"],
            "aot_ticks": aot, "ir_warm_ticks": warm, "ir_cold_ticks": cold,
            "cold_reps": fixture["cold_reps"], "decode_est_ticks": 500 * fixture["cold_reps"],
            "warm_cache_hits": item["hits"], "warm_cache_misses": 0,
            "cold_cache_misses": fixture["cold_reps"],
            "cold_cache_blocks": fixture["cold_reps"], "verified": True,
        })
        csv_lines.append(f"{item['addr']},{item['hits']},{aot}")
    meta = {
        "format_version": 1, "benchmark": fixture["benchmark"],
        "timing_scope": "azahar_emulator", "hardware_time_claim": False,
        "clock_source": "svcGetSystemTick", "tick_hz": fixture["tick_hz"],
        "time_unit": "ticks", "canonical_tier": "aot_warm",
        "canonical_profile": "armtime_profile.csv",
        "profile_semantics": "synthetic test", "cold_semantics": "synthetic test",
        "verification": "pass", "verification_inputs": 2,
        "postflight_verification": "pass", "fixture": "synthetic test",
        "notes": "Azahar emulator timings only; not New 3DS hardware measurements",
        "functions": functions,
        "totals": {
            "hits": sum(item["hits"] for item in functions),
            "aot_ticks": sum(item["aot_ticks"] for item in functions),
            "ir_warm_ticks": sum(item["ir_warm_ticks"] for item in functions),
            "ir_cold_ticks": sum(item["ir_cold_ticks"] for item in functions),
            "decode_est_ticks": sum(item["decode_est_ticks"] for item in functions),
        },
    }
    profile = base / "armtime_profile.csv"
    metadata = base / "armtime_meta.json"
    profile.write_text("\n".join(csv_lines) + "\n", encoding="utf-8")
    metadata.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return profile, metadata, meta


def main():
    with tempfile.TemporaryDirectory(prefix="arm_profile_test_") as td:
        base = Path(td)
        profile, metadata, meta = make_run(base)
        out = base / "selection"
        p = run("check", "--profile", profile, "--metadata", metadata,
                "--fixture", FIXTURE, "--out", out, "--limit", 3)
        assert p.returncode == 0, p.stdout + p.stderr
        report = json.loads((out / "check.json").read_text(encoding="utf-8"))
        manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
        assert manifest["ranking_mode"] == "time"
        assert manifest["profile_time_unit"] == "ticks"
        assert report["selector_partition"]["aot_functions"] == 3
        assert 0 < report["selector_partition"]["covered_time_share"] < 1
        assert "not New 3DS hardware" in report["timing_scope"]
        assert "covered time share" in p.stdout
        print("  ok verified ticks CSV feeds time ranking and reports the split")

        meta["hardware_time_claim"] = True
        metadata.write_text(json.dumps(meta), encoding="utf-8")
        p = run("check", "--profile", profile, "--metadata", metadata,
                "--fixture", FIXTURE, "--out", base / "bad-hardware", "--limit", 3)
        assert p.returncode != 0 and "hardware_time_claim" in p.stderr
        print("  ok hardware timing claims are rejected")

        meta["hardware_time_claim"] = False
        metadata.write_text(json.dumps(meta), encoding="utf-8")
        profile.write_text(profile.read_text(encoding="utf-8").replace(
            str(fixture_first_hits(FIXTURE)), str(fixture_first_hits(FIXTURE) + 1), 1),
            encoding="utf-8")
        p = run("check", "--profile", profile, "--metadata", metadata,
                "--fixture", FIXTURE, "--out", base / "bad-hits", "--limit", 3)
        assert p.returncode != 0 and "CSV does not match" in p.stderr
        print("  ok fixture/CSV hit mismatches are rejected")

        sd = base / "sd"
        sd.mkdir()
        (sd / "Skyrim_N3DS.log").write_text("x" * 1000 + "ARMTIME RESULT: PASS\n", encoding="utf-8")
        azahar_log = base / "azahar_log.txt"
        azahar_log.write_text("x" * 1000 + "ARMTIME RESULT: PASS\n", encoding="utf-8")
        archive = base / "archive"
        p = run("collect", "--sd-root", sd, "--out", archive,
                "--max-log-bytes", "128", "--max-result-bytes", "4096",
                "--azahar-log", azahar_log)
        assert p.returncode != 0  # no profile: collect archives evidence, then reports missing profile
        am = json.loads((archive / "archive_manifest.json").read_text(encoding="utf-8"))
        item = next(item for item in am["files"] if item["name"] == "Skyrim_N3DS.log")
        assert item["truncated"] is False and item["archived_bytes"] < 4096
        tail = next(item for item in am["files"] if item["name"] == "azahar_log_tail.txt")
        assert tail["truncated"] is True and tail["archived_bytes"] < 256
        print("  ok collect bounds emulator log tails and refuses silent result truncation")

    print("arm_profile host tests passed")
    return 0


def fixture_first_hits(fixture_path):
    fixture = json.loads(Path(fixture_path).read_text(encoding="utf-8"))
    return fixture["functions"][0]["hits"]


if __name__ == "__main__":
    raise SystemExit(main())
