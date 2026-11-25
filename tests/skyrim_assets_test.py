"""Tests for the read-only Skyrim asset analysis package (tools/skyrim).

Run with the project venv:
    .venv/Scripts/python.exe tests/skyrim_assets_test.py

The parsers are exercised against synthetic fixtures so the test is fast and
contains no game bytes.  Set ``SKYRIM_N3DS_REAL_DATA=1`` (or to the Data
directory) to also run a handful of read-only checks against the installed
Skyrim archives.
"""

import io
import os
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

from skyrim import bsa, budget, dds, esm, hkx, nif, shaders  # noqa: E402


# ---------------------------------------------------------------------------
# Synthetic BSA writer (mirrors the on-disk layout the reader was verified on)
# ---------------------------------------------------------------------------

def _bstring(text: str, include_nul: bool = True) -> bytes:
    raw = text.encode("cp1252")
    if include_nul:
        raw += b"\x00"
    return bytes([len(raw)]) + raw


def build_bsa(files, version=104, compressed=False, embed=False, name_block=True):
    """files: list of (full_path, bytes) or dict path->bytes."""
    if isinstance(files, dict):
        items = sorted(files.items())
    else:
        items = list(files)
    # Group in path order.
    grouped = []
    by_folder = {}
    for path, data in items:
        folder, _, name = path.rpartition("\\")
        if folder not in by_folder:
            by_folder[folder] = []
            grouped.append(folder)
        by_folder[folder].append((name, data))

    stored = {}
    for folder in grouped:
        for name, data in by_folder[folder]:
            path = (folder + "\\" + name) if folder else name
            prefix = _bstring(path, include_nul=False) if embed else b""
            if compressed:
                payload = prefix + struct.pack("<I", len(data)) + zlib.compress(data)
            else:
                payload = prefix + data
            stored[path] = payload

    flags = 0x001 | 0x002
    if compressed:
        flags |= 0x004
    if embed:
        flags |= 0x100

    folder_recs_size = 16 * len(grouped)
    interleaved_size = sum(
        1 + len(folder.encode("cp1252")) + 1 + 16 * len(by_folder[folder])
        for folder in grouped
    )
    base_names = []
    for folder in grouped:
        for name, _ in by_folder[folder]:
            base_names.append(name)
    name_block_bytes = (
        b"".join(name.encode("cp1252") + b"\x00" for name in base_names)
        if name_block else b""
    )
    data_start = 36 + folder_recs_size + interleaved_size + len(name_block_bytes)

    offsets = {}
    pos = data_start
    for folder in grouped:
        for name, _ in by_folder[folder]:
            path = (folder + "\\" + name) if folder else name
            offsets[path] = pos
            pos += len(stored[path])

    out = bytearray()
    out += struct.pack(
        "<4s8I",
        b"BSA\x00",
        version,
        36,
        flags,
        len(grouped),
        len(items),
        sum(len(folder.encode("cp1252")) + 1 for folder in grouped),
        len(name_block_bytes),
        0,
    )
    for folder in grouped:
        out += struct.pack("<QII", 0, len(by_folder[folder]), 0)
    for folder in grouped:
        out += _bstring(folder)
        for name, _ in by_folder[folder]:
            path = (folder + "\\" + name) if folder else name
            out += struct.pack("<QII", 0, len(stored[path]), offsets[path])
    out += name_block_bytes

    assert len(out) == data_start
    for folder in grouped:
        for name, _ in by_folder[folder]:
            path = (folder + "\\" + name) if folder else name
            assert len(out) == offsets[path]
            out += stored[path]
    return bytes(out)


# ---------------------------------------------------------------------------
# Synthetic DDS writer
# ---------------------------------------------------------------------------

def build_dds(width, height, kind, payload, mip_count=1):
    fourcc = {
        "bc1": b"DXT1", "bc2": b"DXT3", "bc3": b"DXT5",
        "bc4": b"ATI1", "bc5": b"ATI2",
    }.get(kind, b"\x00\x00\x00\x00")
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000
    if mip_count > 1:
        flags |= 0x20000
    pf_flags = 0x4 if kind in ("bc1", "bc2", "bc3", "bc4", "bc5") else 0x40
    bit_count = 0 if pf_flags & 0x4 else 32
    masks = (0, 0, 0, 0)
    if kind == "rgba8":
        masks = (0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    elif kind == "bgr24":
        bit_count = 24
        masks = (0x00FF0000, 0x0000FF00, 0x000000FF, 0)
    elif kind == "rgb24":
        bit_count = 24
        masks = (0x000000FF, 0x0000FF00, 0x00FF0000, 0)
    header = bytearray()
    header += b"DDS "
    header += struct.pack("<7I", 124, flags, height, width, 0, 0, mip_count)
    header += b"\x00" * 44
    header += struct.pack("<8I", 32, pf_flags, struct.unpack("<I", fourcc)[0],
                          bit_count, *masks)
    header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    assert len(header) == 128
    return bytes(header) + payload


def bc1_block(c0, c1, indices):
    return struct.pack("<HHI", c0, c1, indices)


# ---------------------------------------------------------------------------
# Synthetic NIF writer
# ---------------------------------------------------------------------------

NIF_VERSION = 0x14020007


def build_shape_data(num_vertices=4, num_triangles=2, texture_path=None):
    out = bytearray()
    out += struct.pack("<I", 0)                      # group id
    out += struct.pack("<H", num_vertices)
    out += bytes([0, 0, 1])                          # keep, compress, has vertices
    out += b"\x00" * (num_vertices * 12)             # vertices
    out += struct.pack("<H", 0)                      # data flags: no UV/tangents
    out += struct.pack("<I", 0)                      # material CRC
    out += bytes([0])                                # no normals
    out += b"\x00" * 16                              # bounding sphere
    out += bytes([0])                                # no vertex colours
    out += struct.pack("<H", 0)                      # consistency flags
    out += struct.pack("<i", -1)                     # additional data ref
    out += struct.pack("<H", num_triangles)
    out += struct.pack("<I", num_triangles * 3)      # triangle points
    out += bytes([1])                                # has triangles
    out += b"\x00" * (num_triangles * 6)             # indices
    out += struct.pack("<H", 0)                      # match groups
    return bytes(out)


def build_nif(shape_data, texture_path=None, extra_block_padding=b""):
    types = ["NiTriShape", "NiTriShapeData"]
    shape = bytearray()
    shape += struct.pack("<i", -1)                   # name
    shape += struct.pack("<I", 0)                    # extra data count
    shape += struct.pack("<i", -1)                   # controller
    shape += struct.pack("<I", 0x0008000E)           # flags
    shape += b"\x00" * 52                            # transform
    shape += struct.pack("<i", -1)                   # collision
    shape += struct.pack("<i", 1)                    # data ref
    while len(shape) < 97:
        shape.append(0)
    if texture_path:
        shape += texture_path.encode("ascii") + b"\x00"
    blocks = [bytes(shape), shape_data]
    sizes = [len(block) for block in blocks]

    header = bytearray()
    text = b"Gamebryo File Format, Version 20.2.0.7\n"
    assert len(text) == 39
    header += text
    header += struct.pack("<I", NIF_VERSION)
    header += bytes([1])
    header += struct.pack("<I", 12)
    header += struct.pack("<I", len(blocks))
    header += struct.pack("<I", 83)
    header += _bstring("tester")
    header += _bstring("")
    header += _bstring("")
    header += struct.pack("<H", len(types))
    for name in types:
        raw = name.encode("ascii")
        header += struct.pack("<I", len(raw)) + raw
    header += struct.pack("<%dH" % len(blocks), *range(len(blocks)))
    header += struct.pack("<%dI" % len(blocks), *sizes)
    strings = ["shapeName"]
    header += struct.pack("<II", len(strings), max(len(s) for s in strings))
    for text_value in strings:
        raw = text_value.encode("ascii")
        header += struct.pack("<I", len(raw)) + raw
    header += struct.pack("<I", 0)                   # reserved
    body = header + b"".join(blocks) + struct.pack("<II", 1, 0)
    return bytes(body)


# ---------------------------------------------------------------------------
# Synthetic ESM writer
# ---------------------------------------------------------------------------

def _sub(kind, payload):
    return kind + struct.pack("<H", len(payload)) + payload


def _record(kind, form_id, payload, flags=0):
    return kind + struct.pack("<IIIIHH", len(payload), flags, form_id, 0, 0, 0) + payload


def _group(kind, label, children, group_type=0):
    body = b"".join(children)
    header_size = 24
    return (
        b"GRUP"
        + struct.pack("<I", header_size + len(body))
        + label
        + struct.pack("<iHHHH", group_type, 0, 0, 0, 0)
        + body
    )


def build_esm(cell_id=0x00001000, mesh="Architecture\\Test\\Thing.nif"):
    ref = _record(
        b"REFR",
        0x00002000,
        _sub(b"NAME", struct.pack("<I", 0x00003000))
        + _sub(b"DATA", struct.pack("<fff", 1.0, 2.0, 3.0)),
    )
    stat = _record(b"STAT", 0x00003000, _sub(b"MODL", mesh.encode("cp1252") + b"\x00"))
    cell_payload = (
        _sub(b"EDID", b"TestCell\x00")
        + _sub(b"XCLC", struct.pack("<ii", 4, -11))
        + _sub(b"DATA", b"\x00")
    )
    cell = _record(b"CELL", cell_id, cell_payload)
    children = _group(b"CELL", b"\x00\x00\x10\x00", [ref], group_type=6)
    return (
        _record(b"TES4", 0, _sub(b"HEDR", b"\x00" * 12))
        + _group(b"WRLD", b"TEST", [stat, cell, children], group_type=1)
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_bsa():
    files = {
        "textures\\a\\one.dds": b"DDS " + b"A" * 60,
        "textures\\a\\two.dds": b"",
        "meshes\\b\\x.nif": b"Gamebryo" * 9,
    }
    for version in (104, 105):
        for compressed in (False, True):
            for embed in (False, True):
                blob = build_bsa(files, version=version, compressed=compressed, embed=embed)
                with bsa.BsaArchive(io.BytesIO(blob)) as archive:
                    entries = archive.list_files()
                    assert len(entries) == 3
                    assert {e.extension for e in entries} == {"dds", "nif"}
                    for path, data in files.items():
                        assert archive.read_file(path) == data, (version, compressed, embed, path)
                        assert archive.read_file(path.upper()) == data
                        assert archive.read_file(path.replace("\\", "/")) == data
                        assert archive.unpacked_size(path) == len(data)
                        assert archive.read_prefix(path, 4) == data[:4]
                    stat = archive.stat()
                    assert stat["file_count"] == 3
                    assert stat["header"]["version"] == version
                    assert stat["header"]["compressed"] is compressed
                    assert stat["header"]["embedded_names"] is embed
                    assert stat["extensions"]["dds"]["count"] == 2
    print("  ok: BSA v104/v105 x compressed/embedded matrix")

    # No file-name block: embedded names must be used instead.
    blob = build_bsa(files, compressed=True, embed=True, name_block=False)
    with bsa.BsaArchive(io.BytesIO(blob)) as archive:
        assert len(archive.list_files()) == 3
        for path, data in files.items():
            assert archive.read_file(path) == data
    try:
        bsa.BsaArchive(io.BytesIO(build_bsa(files, embed=False, name_block=False)))
    except bsa.BsaFormatError as exc:
        assert "neither" in str(exc)
    else:
        raise AssertionError("nameless non-embedded archive accepted")
    print("  ok: BSA embedded-name-only archives and clear failure otherwise")

    # Rejections.
    bad = bytearray(build_bsa(files))
    bad[0:4] = b"NOPE"
    try:
        bsa.BsaArchive(io.BytesIO(bytes(bad)))
    except bsa.BsaFormatError as exc:
        assert "not a BSA" in str(exc)
    else:
        raise AssertionError("bad magic accepted")
    try:
        bsa.BsaArchive(io.BytesIO(build_bsa(files, version=99)))
    except bsa.BsaFormatError as exc:
        assert "unsupported BSA version" in str(exc)
    else:
        raise AssertionError("version 99 accepted")
    truncated = build_bsa(files, compressed=True)[:-20]
    try:
        bsa.BsaArchive(io.BytesIO(truncated))
    except bsa.BsaFormatError:
        pass
    else:
        raise AssertionError("truncated archive accepted")
    print("  ok: BSA rejects bad magic/version/truncation")


def test_dds():
    # 4x4 BC1: c0=red, c1=blue, all indices 0 -> red, opaque.
    payload = bc1_block(0xF800, 0x001F, 0)
    blob = build_dds(4, 4, "bc1", payload)
    info = dds.parse_dds(blob)
    assert (info.width, info.height, info.mip_count) == (4, 4, 1)
    assert info.format_name == "DXT1"
    w, h, rgba = dds.decode_dds(blob)
    assert (w, h) == (4, 4)
    assert rgba[:4] == bytes((255, 0, 0, 255))
    assert rgba[-4:] == bytes((255, 0, 0, 255))

    # BC2 alpha: 4-bit alpha 0xF everywhere -> 255; colour all green.
    alpha2 = b"\xff" * 8
    color = bc1_block(0x07E0, 0x0000, 0)
    blob = build_dds(4, 4, "bc2", alpha2 + color)
    w, h, rgba = dds.decode_dds(blob)
    assert rgba[0:4] == bytes((0, 255, 0, 255))

    # BC3 alpha: a0=255, a1=0, all alpha indices 0 -> 255; colour all green.
    alpha = bytes([255, 0]) + b"\x00" * 6
    color = bc1_block(0x07E0, 0x0000, 0)
    blob = build_dds(4, 4, "bc3", alpha + color)
    w, h, rgba = dds.decode_dds(blob)
    assert rgba[0:4] == bytes((0, 255, 0, 255))

    # BC4: a0=10, a1=20 -> index 0 gives 10 in every channel.
    blob = build_dds(4, 4, "bc4", bytes([10, 20]) + b"\x00" * 6)
    _, _, rgba = dds.decode_dds(blob)
    assert rgba[:4] == bytes((10, 10, 10, 255))

    # BC5: R=10, G=20.
    blob = build_dds(4, 4, "bc5", bytes([10, 20]) + b"\x00" * 6 + bytes([20, 10]) + b"\x00" * 6)
    _, _, rgba = dds.decode_dds(blob)
    assert rgba[:4] == bytes((10, 20, 0, 255))

    # Two BC1 mips: 8 + 8 bytes.
    two = bc1_block(0xF800, 0, 0) * 2
    blob = build_dds(4, 4, "bc1", two, mip_count=2)
    info = dds.parse_dds(blob)
    assert info.mip_count == 2
    assert [level.size for level in info.levels] == [8, 8]
    assert info.all_mips_size == 16

    # RGBA8 and 24-bit BGR.
    pixels = bytes((1, 2, 3, 4)) * 4
    blob = build_dds(2, 2, "rgba8", pixels)
    _, _, rgba = dds.decode_dds(blob)
    assert rgba == pixels
    bgr = bytes((10, 20, 30)) * 4
    blob = build_dds(2, 2, "bgr24", bgr)
    _, _, rgba = dds.decode_dds(blob)
    assert rgba[:4] == bytes((30, 20, 10, 255))

    assert dds.etc1_mipped_size(512, 512, 1) == 512 * 512 // 2
    assert dds.etc1_mipped_size(512, 512, 10) == 174776
    assert dds.etc1a4_mipped_size(4, 4, 1) == 16
    print("  ok: DDS header + BC1/BC2/BC3/BC4/BC5/RGBA8/BGR24 decode + ETC1 sizing")


def test_nif():
    shape_data = build_shape_data(4, 2, texture_path="textures\\test\\thing.dds")
    blob = build_nif(shape_data, texture_path="textures\\test\\thing.dds")
    parsed = nif.parse_nif(blob)
    assert parsed.version == NIF_VERSION
    assert parsed.user_version == 12
    assert parsed.bs_version == 83
    assert len(parsed.blocks) == 2
    assert parsed.histogram() == {"NiTriShape": 1, "NiTriShapeData": 1}
    geo = parsed.geometry_stats()
    assert geo["totals"]["vertices"] == 4
    assert geo["totals"]["triangles"] == 2
    assert geo["totals"]["shapes"] == 1
    assert parsed.texture_paths() == ["textures\\test\\thing.dds"]
    # The block table must account for every byte.
    assert parsed.header_size > 0
    print("  ok: NIF header, block histogram, NiTriShapeData counts, texture scan")

    # Unsupported geometry is reported, not guessed.
    blob2 = bytearray(blob)
    assert nif.scan_texture_paths(blob2) == ["textures\\test\\thing.dds"]
    print("  ok: NIF texture scan is byte-level")


def test_hkx():
    header = bytearray()
    header += struct.pack("<II", 0x57E0E057, 0x10C0C010)
    header += struct.pack("<IIIIIIII", 0, 8, 0x01000104, 3, 2, 0, 0, 75)
    header += b"hk_2010.2.0-r1\x00"
    header += struct.pack("<I", 0)
    header += b"\x00" * 8
    info = hkx.classify(bytes(header))
    assert info.kind == "packed-binary"
    assert info.file_version == 8
    assert info.num_sections == 3
    assert info.contents_version == "hk_2010.2.0-r1"
    xml = hkx.classify(b'<?xml version="1.0"?>\n<hkpackfile/>')
    assert xml.kind == "xml"
    unknown = hkx.classify(b"not a havok file")
    assert unknown.kind == "unknown"
    print("  ok: HKX packed-binary/XML/unknown classification")


def test_esm_and_budget():
    blob = build_esm()
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "Test.esm")
        with open(path, "wb") as handle:
            handle.write(blob)
        scan = esm.scan_plugin(path)
        assert len(scan.cells) == 1
        cell = scan.cells[0]
        assert cell.editor_id == "TestCell"
        assert cell.grid == (4, -11)
        assert cell.ref_count == 1
        assert scan.base_meshes[0x00003000].lower().endswith("thing.nif")
        files = esm.extract_cell(path, editor_id="TestCell", scan=scan)
        assert files.mesh_paths == ["meshes\\Architecture\\Test\\Thing.nif"]

        # Compressed CELL payload.
        compressed = zlib.compress(_sub(b"EDID", b"Packed\x00") + _sub(b"XCLC", struct.pack("<ii", 1, 2)))
        raw = struct.pack("<I", len(_sub(b"EDID", b"Packed\x00") + _sub(b"XCLC", struct.pack("<ii", 1, 2)))) + compressed
        packed_blob = _record(b"CELL", 0x00001001, raw, flags=0x00040000)
        path2 = os.path.join(tmp, "Packed.esm")
        with open(path2, "wb") as handle:
            handle.write(packed_blob)
        scan2 = esm.scan_plugin(path2)
        assert scan2.cells[0].editor_id == "Packed"
        assert scan2.cells[0].grid == (1, 2)
    print("  ok: ESM CELL/REFR/MODL extraction and compressed records")

    # LRU: capacity 100, requests A(40), B(30), A, C(50)
    stats = budget.simulate_lru([("A", 40), ("B", 30), ("A", 40), ("C", 50)], 100)
    assert stats.requests == 4
    assert stats.misses == 3
    assert stats.hits == 1
    assert stats.evictions == 1  # B evicted to make room for C
    assert stats.peak_bytes == 90
    oversized = budget.simulate_lru([("big", 200)], 100)
    assert oversized.oversized == 1 and oversized.final_entries == 0
    vram = budget.vram_summary()
    assert vram["surfaces_512x512_top"] == 32
    assert vram["surfaces_512x512_mips"] == 23

    mesh = budget.MeshStats(path="meshes\\a.nif", vertices=100, triangles=50)
    assert mesh.geometry_bytes == 100 * 24 + 50 * 3 * 2
    tex = budget.TextureStats(
        path="textures\\a.dds", width=512, height=512, format="DXT1",
        mips=10, bc_bytes=174776, etc1_bytes=174776, etc1a4_bytes=349552,
    )
    ws = budget.CellWorkingSet(cell={}, mesh_stats=[mesh], texture_stats=[tex])
    report = ws.assess()
    assert report["geometry_bytes"] == mesh.geometry_bytes
    assert report["texture_etc1_bytes"] == 174776
    print("  ok: LRU model, VRAM arithmetic, working-set aggregates")


def test_shaders():
    def instr(opcode, extra=0):
        return struct.pack("<I", (extra << 24) | opcode)

    def chunk(tag, code, meta=0):
        return tag + struct.pack("<II", meta, len(code)) + code

    # vs_3_0 + CTAB comment + mov + dcl + m4x4 + texld + END
    code = struct.pack("<I", 0xFFFE0300)
    comment_body = b"CTAB" + b"\x00" * 8
    code += struct.pack("<I", (len(comment_body) // 4) << 16 | 0xFFFE) + comment_body
    code += instr(0x01, 1) + b"\x00" * 4     # mov
    code += instr(0x1F, 2) + b"\x00" * 8     # dcl
    code += instr(0x14, 2) + b"\x00" * 8     # m4x4
    code += instr(0x42, 3) + b"\x00" * 12    # texld
    code += struct.pack("<I", 0xFFFF)
    blob = chunk(b"BSVS", code)
    chunks = shaders.parse_fxp("test.fxp", blob)
    assert len(chunks) == 1
    c = chunks[0]
    assert c.version_name == "vs_3_0"
    assert c.has_ctab
    assert c.texture_samples == 1
    assert c.uses_port_subset  # exactly mov/dcl/m4x4/texld
    # ps_3_0 with mul -> outside the subset.
    code2 = struct.pack("<I", 0xFFFF0300) + instr(0x05, 1) + b"\x00" * 4 + struct.pack("<I", 0xFFFF)
    chunks2 = shaders.parse_fxp("test2.fxp", chunk(b"BSPS", code2, meta=0x100))
    assert chunks2[0].version_name == "ps_3_0"
    assert not chunks2[0].uses_port_subset
    assert shaders.OPCODES[0x58] == "cmp"
    assert shaders.OPCODES[0x42] == "texld"
    print("  ok: FXP chunk discovery, D3D9 tokenizer, subset classification")


def test_convert_png():
    from skyrim import convert
    import tempfile

    info = build_dds(2, 2, "rgba8", bytes((1, 2, 3, 4)) * 4)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "x.png")
        result = convert.dds_to_png(info, out)
        assert result["width"] == 2 and result["height"] == 2
        data = open(out, "rb").read()
        assert data.startswith(b"\x89PNG\r\n\x1a\n")
        assert b"IHDR" in data and b"IEND" in data
    print("  ok: DDS -> PNG writer")


def test_real_data():
    configured = os.environ.get("SKYRIM_N3DS_REAL_DATA") or os.environ.get("SKYRIM_N3DS_DATA")
    if not configured:
        print("  skip: real-data checks (set SKYRIM_N3DS_REAL_DATA=1 to enable)")
        return
    data_dir = (
        configured
        if os.path.isdir(configured)
        else r"E:\SteamLibrary\steamapps\common\Skyrim\Data"
    )
    if not os.path.isdir(data_dir):
        print("  skip: real data directory not found")
        return
    textures = os.path.join(data_dir, "Skyrim - Textures.bsa")
    meshes = os.path.join(data_dir, "Skyrim - Meshes.bsa")
    shader_bsa = os.path.join(data_dir, "Skyrim - Shaders.bsa")
    animations = os.path.join(data_dir, "Skyrim - Animations.bsa")
    with bsa.open_archive(textures) as archive:
        entries = archive.list_files()
        assert len(entries) > 18000
        assert sum(1 for e in entries if e.extension == "dds") > 18000
        dog = archive.find("textures\\actors\\dog\\dog.dds")
        assert dog is not None
        prefix = archive.read_prefix("textures\\actors\\dog\\dog.dds", 148)
        info = dds.parse_dds(prefix, require_payload=False)
        assert (info.width, info.height) == (1024, 1024)
        assert info.format_name == "DXT5"
        assert info.mip_count == 11
        assert dds.etc1_mipped_size(info.width, info.height, info.mip_count) == 699064
    print("  ok: real Textures.bsa (%d files) and dog.dds header" % len(entries))
    with bsa.open_archive(meshes) as archive:
        sample = archive.find("meshes\\dungeons\\nordic\\levers\\pullchainanim\\norpullchainanim01.nif")
        assert sample is not None
        parsed = nif.parse_nif(archive.read_file(sample.path))
        geo = parsed.geometry_stats()
        assert parsed.histogram().get("NiTriShapeData") == 8
        assert geo["totals"]["vertices"] == 5687
        assert geo["totals"]["triangles"] == 8538
    print("  ok: real Meshes.bsa NIF header/geometry")
    if os.path.isfile(shader_bsa):
        with bsa.open_archive(shader_bsa) as archive:
            inventory = shaders.inventory_archive(archive)
        summary = inventory.summary()
        assert summary["file_count"] == 122
        assert summary["extensions"] == {"fxp": 122}
        assert summary["chunk_count"] > 10000
        assert summary["versions"] == {"vs_3_0": 854, "ps_3_0": 9621}
        assert summary["source_files"] == 0
    print("  ok: real Shaders.bsa inventory")
    if os.path.isfile(animations):
        with bsa.open_archive(animations) as archive:
            entry = next(e for e in archive.list_files() if e.extension == "hkx")
            info = hkx.classify(archive.read_file(entry.path))
        assert info.kind == "packed-binary"
        assert info.contents_version == "hk_2010.2.0-r1"
        assert info.num_sections == 3
    print("  ok: real HKX classification")


def main():
    print("skyrim asset tests")
    test_bsa()
    test_dds()
    test_nif()
    test_hkx()
    test_esm_and_budget()
    test_shaders()
    test_convert_png()
    test_real_data()
    print("skyrim assets: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
