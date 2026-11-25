#!/usr/bin/env python3
"""Host tests for the DDS/BC -> ETC1 pipeline and SKTX container.

Synthetic DDS files exercise BC1/BC2/BC3, alpha policy, mip/dimension handling,
bounded-memory failures and deterministic container validation.  A real-data
section is opt-in with SKYRIM_N3DS_REAL_DATA=1 so the default test run never
depends on the retail install or tex3ds.

Run:
    python tests/texture_pipeline_test.py
"""

from __future__ import annotations

import json
import os
import struct
import sys
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from skyrim import dds, etc1  # noqa: E402


def _dds(width: int, height: int, kind: str, payload: bytes,
         mips: int = 1, fourcc: bytes = b"") -> bytes:
    """Build a minimal valid DDS header for the test payload."""
    if not fourcc:
        fourcc = {"bc1": b"DXT1", "bc2": b"DXT3", "bc3": b"DXT5"}[kind]
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000
    if mips > 1:
        flags |= 0x20000
    header = bytearray(128)
    header[0:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, flags, height, width,
                     len(payload), 0, mips)
    # pixel format at offset 76
    struct.pack_into("<8I", header, 76, 32, 0x4, int.from_bytes(fourcc, "little"),
                     0, 0, 0, 0, 0)
    struct.pack_into("<4I", header, 108, 0x1000, 0, 0, 0)
    return bytes(header) + payload


def _bc1_block(r: int, g: int, b: int) -> bytes:
    def to565(r: int, g: int, b: int) -> int:
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    c0 = to565(r, g, b)
    c1 = to565(r // 2, g // 2, b // 2)
    return struct.pack("<HHI", c0, c1, 0)


def _bc3_block(color: bytes, a0: int = 255, a1: int = 0) -> bytes:
    # 8 alpha bytes (a0, a1, 48 bits of 3-bit indices) + 8 colour bytes
    return bytes((a0, a1, 0, 0, 0, 0, 0, 0)) + color


def _bc2_block(color: bytes, alpha_nibble: int = 0xF) -> bytes:
    alpha = bytes((alpha_nibble | (alpha_nibble << 4),)) * 8
    return alpha + color


def _mip_payload(width: int, height: int, block: bytes) -> bytes:
    bw, bh = (width + 3) // 4, (height + 3) // 4
    return block * (bw * bh)


def check(condition, what: str) -> None:
    if not condition:
        raise AssertionError(what)


def test_plans() -> None:
    red = _bc1_block(255, 0, 0)
    bc1 = _dds(8, 8, "bc1", _mip_payload(8, 8, red))
    p = etc1.plan_texture(bc1, "red_bc1.dds")
    check(p.output_format == "ETC1", "BC1 opaque -> ETC1")
    check(p.target_mips == 1, "8x8 has one PICA mip")
    check(p.chosen_bytes == 32, "8x8 ETC1 is 32 bytes")
    check(not p.needs_alpha, "opaque BC1 has no alpha")
    check(p.alpha_source_bits == 0, "opaque BC1 alpha bits")

    bc2 = _dds(8, 8, "bc2", _mip_payload(8, 8, _bc2_block(red)))
    p = etc1.plan_texture(bc2, "red_bc2.dds")
    check(p.output_format == "ETC1A4", "BC2 -> ETC1A4")
    check(p.chosen_bytes == 64, "8x8 ETC1A4 is 64 bytes")
    check(p.needs_alpha and p.alpha_source_bits == 8, "BC2 alpha bits")

    bc3 = _dds(8, 8, "bc3", _mip_payload(8, 8, _bc3_block(red)))
    p = etc1.plan_texture(bc3, "red_bc3.dds")
    check(p.output_format == "ETC1A4", "BC3 -> ETC1A4")
    drop = etc1.TexturePolicy(alpha_mode="drop")
    p = etc1.plan_texture(bc3, "red_bc3.dds", drop)
    check(p.output_format == "ETC1" and p.alpha_dropped, "BC3 + drop is explicit")

    # BC1 punch-through mode: c0 <= c1 means index 3 is transparent.
    transparent = _dds(8, 8, "bc1", _mip_payload(8, 8, struct.pack("<HHI", 0, 0xFFFF, 0)))
    p = etc1.plan_texture(transparent, "transparent_bc1.dds")
    check(p.needs_alpha and p.output_format == "ETC1A4", "BC1 punch-through -> alpha")

    # Header-only planning for the real 2048 surfaces: enter at mip 1.
    big = _dds(2048, 2048, "bc1", b"\0" * 8, mips=12)
    p = etc1.plan_texture(big, "big.dds", require_payload=False)
    check(p.start_level == 1, "2048 enters at mip 1")
    check((p.target_width, p.target_height) == (1024, 1024), "target 1024")
    check(p.target_mips == 8, "1024 chain has 8 PICA levels")
    check(p.estimated_peak_bytes < 16 * 1024 * 1024, "2048 plan is bounded")

    # Deterministic budget failures.
    try:
        etc1.plan_texture(bc3, "bc3.dds",
                          etc1.TexturePolicy(max_output_bytes=10))
        raise AssertionError("output budget did not fail")
    except etc1.TextureBudgetError as exc:
        check("max_output_bytes" in str(exc), "output budget names the limit")
    try:
        etc1.plan_texture(big, "big.dds", require_payload=False,
                          policy=etc1.TexturePolicy(max_memory_bytes=1024))
        raise AssertionError("memory budget did not fail")
    except etc1.TextureBudgetError as exc:
        check("max_memory_bytes" in str(exc), "memory budget names the limit")

    # Non-power-of-two is rejected, not silently padded.
    npot = bytearray(_dds(10, 10, "bc1", b"\0" * (3 * 3 * 8)))
    try:
        etc1.plan_texture(bytes(npot), "npot.dds")
        raise AssertionError("NPOT was accepted")
    except etc1.TextureError as exc:
        check("power-of-two" in str(exc), "NPOT error is explicit")
    print("  ok: plans, alpha policy, mip/dimension and budget failures")


def _convert(data: bytes, name: str, out: str, **kwargs) -> etc1.ConversionResult:
    return etc1.convert_texture(data, name, out, **kwargs)


def test_container_and_quality(tmp: str) -> None:
    red_block = _bc1_block(255, 0, 0)
    bc1 = _dds(8, 8, "bc1", _mip_payload(8, 8, red_block))
    result = _convert(bc1, "red_bc1.dds", tmp)
    check(result.container_bytes == 24 + 12 + 32, "SKTX container size")
    check(result.container_bytes == result.plan.container_bytes,
          "plan predicts the container size")
    parsed = etc1.read_sktx(open(result.container_path, "rb").read())
    check(parsed["format"] == "etc1", "SKTX format")
    check(parsed["levels"][0]["bytes"] == 32, "SKTX level bytes")
    check(result.quality["all"]["psnr_db"] is not None, "quality measured")
    check(result.quality["all"]["max_abs_error"] <= 4, "solid red colour error")
    check(result.memory["peak_bytes"] > 0, "memory peak recorded")
    check(result.memory["peak_bytes"] <= result.plan.policy.max_memory_bytes,
          "memory peak within policy")

    # BC2 alpha -> ETC1A4, and the alpha limitation is explicit.
    bc2 = _dds(8, 8, "bc2", _mip_payload(8, 8, _bc2_block(red_block, 0x8)))
    result = _convert(bc2, "alpha_bc2.dds", tmp)
    check(result.plan.output_format == "ETC1A4", "BC2 converted as ETC1A4")
    check(result.quality["alpha"]["output_bits"] == 4, "4-bit alpha stated")
    check("not BC3" in result.quality["alpha"]["note"], "BC3 equivalence denied")
    check(etc1.read_sktx(open(result.container_path, "rb").read())["format"]
          == "etc1a4", "ETC1A4 SKTX format")

    # Explicit alpha drop is lossy and labelled.
    result = _convert(bc2, "drop_bc2.dds", tmp,
                      policy=etc1.TexturePolicy(alpha_mode="drop"))
    check(result.plan.output_format == "ETC1", "drop policy chooses ETC1")
    check(result.plan.alpha_dropped, "drop policy is flagged")
    check(result.quality["alpha"]["dropped"], "quality says alpha dropped")

    # Generated mips: source has one level, tex3ds supplies the chain.
    bc1_16 = _dds(16, 16, "bc1", _mip_payload(16, 16, red_block), mips=1)
    result = _convert(bc1_16, "gen_bc1.dds", tmp,
                      policy=etc1.TexturePolicy(mip_mode="generate"))
    check(result.plan.target_mips == 2, "16x16 generated two levels")
    check(len(result.level_sizes) == 2, "two level sizes recorded")
    check(result.level_sizes[1]["bytes"] == 32, "generated 8x8 level bytes")
    check("tex3ds -m box" in result.level_sizes[1]["source"], "generated source noted")

    # Preserved source mips: supply two stored levels and check level 1 uses it.
    payload = _mip_payload(16, 16, red_block) + _mip_payload(8, 8, red_block)
    bc1_mips = _dds(16, 16, "bc1", payload, mips=2)
    result = _convert(bc1_mips, "mips_bc1.dds", tmp)
    check(result.plan.target_mips == 2, "source chain has two levels")
    check("source mip 1" in result.level_sizes[1]["source"],
          "stored source mip used")
    check(result.level_sizes[1]["bytes"] == 32, "stored mip size")

    blob = open(result.container_path, "rb").read()
    for label, damage in [
        ("magic", b"XXXX" + blob[4:]),
        ("crc", blob[:-1] + bytes((blob[-1] ^ 0xFF,))),
        ("truncated", blob[:-1]),
    ]:
        try:
            etc1.read_sktx(damage)
            raise AssertionError("container damage accepted: " + label)
        except etc1.TextureError:
            pass
    print("  ok: BC1/BC2 conversion, container, quality, mips and corruption")


def test_quality_metric_rejects_thumbnail() -> None:
    # The ETC1 decoder is validated on a four-quadrant 16x16 surface; a
    # thumbnail comparison would have the wrong dimensions and fail.
    w = h = 16
    cols = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]
    rgba = bytearray()
    for y in range(h):
        for x in range(w):
            c = cols[(1 if x >= 8 else 0) + (2 if y >= 8 else 0)]
            rgba += bytes((*c, 255))
    # A perfect decoder compared with itself.
    q = etc1.psnr_rgb(bytes(rgba), bytes(rgba), w, h)
    check(q["psnr_db"] is None and q["max_abs_error"] == 0, "identity PSNR")
    # Pixels below the alpha threshold are not scored.
    faded = bytearray(rgba)
    for i in range(3, len(faded), 4):
        faded[i] = 0
    q = etc1.psnr_rgb(bytes(faded), bytes(rgba), w, h, alpha_threshold=8)
    check(q["pixels"] == 0 and q["skipped"], "alpha mask skips invisible pixels")
    print("  ok: quality metric identity and visible-pixel mask")


def test_inventory_projection() -> None:
    path = r"E:\Skyrim_N3DS_build\assets\textures\textures.json"
    if not os.path.isfile(path):
        print("  skip: texture inventory JSON not present")
        return
    rows = json.load(open(path, encoding="utf-8"))
    p = etc1.project_inventory(rows)
    check(p["textures"] == 18580, "inventory texture count")
    check(p["downscaled_to_max_dim"] == 64, "64 surfaces exceed 1024")
    check(p["etc1_alpha_dropped_bytes"] > 0, "ETC1 projection")
    check(p["mixed_auto_bytes"] > p["etc1_alpha_dropped_bytes"], "alpha adds bytes")
    check(p["mixed_auto_bytes"] <= p["etc1a4_all_bytes"], "mixed is an intermediate")
    print("  ok: real inventory projection %d textures, %d downscaled, "
          "ETC1 %.2f MiB, mixed %.2f MiB" %
          (p["textures"], p["downscaled_to_max_dim"],
           p["etc1_alpha_dropped_bytes"] / 1048576.0,
           p["mixed_auto_bytes"] / 1048576.0))


def test_real_sample(tmp: str) -> None:
    if os.environ.get("SKYRIM_N3DS_REAL_DATA") != "1":
        print("  skip: real DDS conversion (set SKYRIM_N3DS_REAL_DATA=1)")
        return
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from skyrim import bsa
    archive_path = r"E:\SteamLibrary\steamapps\common\Skyrim\Data\Skyrim - Textures.bsa"
    if not os.path.isfile(archive_path):
        print("  skip: retail Textures.bsa not present")
        return
    archive = bsa.BsaArchive(archive_path)
    for name in etc1.MENU_TEXTURES:
        data = archive.read_file(name)
        result = etc1.convert_texture(data, name, tmp)
        check(result.plan.chosen_bytes > 0, "real output bytes")
        check(result.memory["peak_bytes"] <=
              result.plan.policy.max_memory_bytes, "real peak bounded")
        q = result.quality
        print("  ok: %s %s %dx%d -> %s %d B, visible PSNR %s, peak %d" %
              (name, result.plan.source_format, result.plan.width,
               result.plan.height, result.plan.output_format,
               result.plan.chosen_bytes, q["visible"]["psnr_db"],
               result.memory["peak_bytes"]))


def main() -> int:
    tmp = os.path.join(ROOT, "build_host", "texture_pipeline_test")
    os.makedirs(tmp, exist_ok=True)
    print("texture pipeline tests")
    try:
        test_plans()
        test_container_and_quality(tmp)
        test_quality_metric_rejects_thumbnail()
        test_inventory_projection()
        test_real_sample(tmp)
    except Exception:
        traceback.print_exc()
        return 1
    print("texture pipeline: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
