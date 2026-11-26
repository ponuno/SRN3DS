"""Tests the pack-plan merge path in tools/fxp_compile.py.

Pack agents own ``source/shader_packs/<pack>/plans.json``.  The generator can
only produce its four canonical entries, so every other plan reaches
``docs/shader_plans.json`` through ``merge_extra_entries``.  These checks pin
the contract: extras only add, duplicates are idempotent, and a conflict is a
named error rather than a silent overwrite.
"""

import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "shaders"))

import fxp_compile  # noqa: E402
import plan_verify  # noqa: E402


def entry(name, sha_char, draw=0):
    return {"id": name, "sha256": sha_char * 64, "kind": "tev",
            "stage": "ps_3_0", "family": "terrain", "draw_enabled": draw,
            "stages": 2, "stage_data": []}


def raises(fn, needle):
    try:
        fn()
    except SystemExit as exc:
        assert needle in str(exc), str(exc)
        return
    raise AssertionError("expected SystemExit containing %r" % needle)


def main():
    checks = 0
    canonical = {"schema": "d9-shader-plan-v1",
                 "entries": [entry("menu.copy.ps", "a", draw=1)]}
    extra_a = {"entries": [entry("terrain.simple.ps", "b")]}
    extra_b = {"entries": [entry("terrain.lod.ps", "c")]}

    merged = fxp_compile.merge_extra_entries(
        canonical, [("pack:terrain", extra_a), ("pack:terrain", extra_b)])
    assert [row["id"] for row in merged["entries"]] == [
        "menu.copy.ps", "terrain.simple.ps", "terrain.lod.ps"]
    assert len(canonical["entries"]) == 1, "merge must not mutate the input"
    checks += 2

    again = fxp_compile.merge_extra_entries(
        canonical, [("pack:terrain", extra_a), ("pack:terrain", extra_a)])
    assert len(again["entries"]) == 2, "byte-identical duplicates are idempotent"
    checks += 1

    catalogue = {"entries": [entry("terrain.simple.ps", "b"),
                             dict(entry("terrain.lod.ps", "c"), runtime=1)]}
    assert [row["id"] for row in fxp_compile.runtime_entries(catalogue)] == [
        "terrain.lod.ps"]
    runtime = fxp_compile.merge_extra_entries(
        canonical, [("pack:terrain", catalogue)], runtime_only=True)
    assert [row["id"] for row in runtime["entries"]] == [
        "menu.copy.ps", "terrain.lod.ps"], runtime["entries"]
    checks += 2

    clash = {"entries": [dict(entry("menu.copy.ps", "a", draw=1),
                              note="pack rewrite")]}
    raises(lambda: fxp_compile.merge_extra_entries(
        canonical, [("pack:terrain", clash)]), "conflicts with the committed")
    checks += 1

    dup_id = {"entries": [dict(extra_a["entries"][0], sha256="d" * 64)]}
    raises(lambda: fxp_compile.merge_extra_entries(
        canonical, [("pack:terrain", extra_a), ("pack:x", dup_id)]),
        "duplicate plan id")
    checks += 1

    raises(lambda: fxp_compile.merge_extra_entries(
        canonical, [("pack:terrain", {"entries": [dict(
            extra_a["entries"][0], sha256="B" * 64)]})]), "64 lower-case hex")
    raises(lambda: fxp_compile.merge_extra_entries(
        canonical, [("pack:terrain", {"entries": [dict(
            extra_a["entries"][0], kind="ps")]})]), "has kind")
    checks += 2

    with tempfile.TemporaryDirectory() as tmp:
        packs = Path(tmp) / "shader_packs"
        (packs / "zz").mkdir(parents=True)
        (packs / "aa").mkdir()
        (packs / "aa" / "plans.json").write_text('{"entries": []}', encoding="utf-8")
        (packs / "zz" / "manifest.json").write_text("{}", encoding="utf-8")
        original_packs = fxp_compile.PACKS_DIR
        fxp_compile.PACKS_DIR = str(packs)
        try:
            found = fxp_compile.pack_plan_paths()
        finally:
            fxp_compile.PACKS_DIR = original_packs
        assert [Path(p).parent.name for p in found] == ["aa"], found
    checks += 1

    # Symbolic layer: the committed document is canonical + extras, so the
    # verifier must merge extras into its regeneration or reject the file.
    original = fxp_compile.build_manifest
    fxp_compile.build_manifest = lambda data_dir="", include_update=False: json.loads(
        json.dumps(canonical))
    try:
        with tempfile.TemporaryDirectory() as tmp:
            doc = fxp_compile.merge_extra_entries(
                canonical, [("pack:terrain", catalogue)], runtime_only=True)
            good = plan_verify.verify_against_archive(
                doc, tmp, [("pack:terrain", catalogue)])
            assert good["status"] == "pass", good
            bad = plan_verify.verify_against_archive(doc, tmp)
            assert bad["status"] == "fail", bad
            assert any("regenerated manifest has no plan" in p
                       for p in bad["problems"])
    finally:
        fxp_compile.build_manifest = original
    checks += 3

    print("shader_plan_merge_test: %d checks OK" % checks)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
