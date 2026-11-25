"""Inventory of Skyrim's shipped shader packages (``Skyrim - Shaders.bsa``).

Findings that drive this module (measured 2026-11-24, see docs/ASSETS.md):

* ``Skyrim - Shaders.bsa`` contains **122 files and every one is a ``.fxp``**
  (compiled shader package).  There are no ``.hlsl``/``.fx``/``.vsh``/``.psh``
  sources in the retail archive, so the "technique/pass/macro" source table is
  necessarily empty.  Each ``.fxp`` embeds D3D9 shader bytecode chunks tagged
  ``BSVS`` (vertex) or ``BSPS`` (pixel).
* Chunk layout inside ``.fxp``::

      b"BSVS" | b"BSPS"
      u32 meta          (vertex: 0; pixel: shader flags)
      u32 size          (bytes of the following D3D9 bytecode blob)
      bytecode[size]    starts with the D3D9 version token

  The tag is followed by a 4-byte metadata word and a 4-byte size; the D3D9
  bytecode begins immediately afterwards and begins with the version token
  (0xFFFE0300 = vs_3_0, 0xFFFF0300 = ps_3_0, ...).  Chunks are packed with
  other data between them, so discovery scans for validated tags rather than
  assuming a headerless array.

This module extracts metadata and an opcode histogram only; it does not decode
shader semantics or re-compile anything.
"""

from __future__ import annotations

import hashlib
import re
import struct
from collections import Counter
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

CHUNK_TAGS = {b"BSVS": "vs", b"BSPS": "ps"}

# D3D9 shader opcodes (d3d9types.h D3DSIO_*).  Names as used by the assembler.
# Note that the texture block starts at 64 (0x40), not 49: TEX=66, TEXLD is
# spelled "texld" in assembly, CMP=88, TEXLDD=93, TEXLDL=95, BREAKP=96.
OPCODES = {
    0x00: "nop", 0x01: "mov", 0x02: "add", 0x03: "sub", 0x04: "mad",
    0x05: "mul", 0x06: "rcp", 0x07: "rsq", 0x08: "dp3", 0x09: "dp4",
    0x0A: "min", 0x0B: "max", 0x0C: "slt", 0x0D: "sge", 0x0E: "exp",
    0x0F: "log", 0x10: "lit", 0x11: "dst", 0x12: "lrp", 0x13: "frc",
    0x14: "m4x4", 0x15: "m4x3", 0x16: "m3x4", 0x17: "m3x3", 0x18: "m3x2",
    0x19: "call", 0x1A: "callnz", 0x1B: "loop", 0x1C: "ret", 0x1D: "endloop",
    0x1E: "label", 0x1F: "dcl", 0x20: "pow", 0x21: "crs", 0x22: "sgn",
    0x23: "abs", 0x24: "nrm", 0x25: "sincos", 0x26: "rep", 0x27: "endrep",
    0x28: "if", 0x29: "ifc", 0x2A: "else", 0x2B: "endif", 0x2C: "break",
    0x2D: "breakc", 0x2E: "mova", 0x2F: "defb", 0x30: "defi",
    0x40: "texcoord", 0x41: "texkill", 0x42: "texld", 0x43: "texbem",
    0x44: "texbeml", 0x45: "texreg2ar", 0x46: "texreg2gb", 0x47: "texm3x2pad",
    0x48: "texm3x2tex", 0x49: "texm3x3pad", 0x4A: "texm3x3tex",
    0x4B: "texm3x3diff", 0x4C: "texm3x3spec", 0x4D: "texm3x3vspec",
    0x4E: "expp", 0x4F: "logp", 0x50: "cnd", 0x51: "def",
    0x52: "texreg2rgb", 0x53: "texdp3tex", 0x54: "texm3x2depth",
    0x55: "texdp3", 0x56: "texm3x3", 0x57: "texdepth", 0x58: "cmp",
    0x59: "bem", 0x5A: "dp2add", 0x5B: "dsx", 0x5C: "dsy",
    0x5D: "texldd", 0x5E: "setp", 0x5F: "texldl", 0x60: "breakp",
}
COMMENT = 0xFFFE
END = 0xFFFF

TEXTURE_SAMPLE_OPS = {0x42, 0x5D, 0x5F}  # texld, texldd, texldl
BRANCH_OPS = {0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x26, 0x27, 0x28, 0x29, 0x2A,
              0x2B, 0x2C, 0x2D, 0x60}
LOOP_OPS = {0x1B, 0x1D, 0x26, 0x27}
CALL_OPS = {0x19, 0x1A}
# The two exact templates the 3DS port translates today:
PORT_SUBSET = {0x01, 0x14, 0x1F, 0x42}  # mov, m4x4, dcl, texld
PORT_SUBSET_LOOSE = PORT_SUBSET | {0x00, 0x51}  # + nop, def


def _u32(data: bytes, pos: int) -> int:
    return struct.unpack_from("<I", data, pos)[0]


@dataclass
class ShaderChunk:
    source: str
    offset: int
    kind: str                 # "vs" or "ps"
    meta: int
    size: int
    version_token: int
    version_name: str
    opcodes: List[int]
    opcode_counts: Dict[int, int]
    instruction_count: int
    comment_count: int
    creator: str
    has_ctab: bool
    texture_samples: int
    branch_count: int
    loop_count: int
    call_count: int
    mrt_writes: int
    bytecode_sha1: str
    program_sha1: str
    error: Optional[str] = None

    @property
    def uses_port_subset(self) -> bool:
        return self.error is None and set(self.opcodes) <= PORT_SUBSET

    @property
    def uses_port_subset_loose(self) -> bool:
        return self.error is None and set(self.opcodes) <= PORT_SUBSET_LOOSE

    def as_dict(self) -> dict:
        return {
            "source": self.source,
            "offset": self.offset,
            "kind": self.kind,
            "meta": self.meta,
            "size": self.size,
            "version_token": "0x%08x" % self.version_token,
            "version": self.version_name,
            "instruction_count": self.instruction_count,
            "comment_count": self.comment_count,
            "creator": self.creator,
            "has_ctab": self.has_ctab,
            "texture_samples": self.texture_samples,
            "branches": self.branch_count,
            "loops": self.loop_count,
            "calls": self.call_count,
            "mrt_writes": self.mrt_writes,
            "bytecode_sha1": self.bytecode_sha1,
            "program_sha1": self.program_sha1,
            "port_subset_exact": self.uses_port_subset,
            "port_subset_loose": self.uses_port_subset_loose,
            "error": self.error,
        }


def version_name(token: int) -> str:
    kind = "vs" if (token >> 16) == 0xFFFE else "ps" if (token >> 16) == 0xFFFF else "unknown"
    major = (token >> 8) & 0xFF
    minor = token & 0xFF
    return "%s_%d_%d" % (kind, major, minor)


def _parse_bytecode(code: bytes) -> dict:
    """Tokenize a D3D9 shader program.

    Returns the opcode list and a few structural counts.  The version token is
    the first DWORD of ``code`` and is skipped; instruction tokens follow.
    """
    if len(code) < 8:
        return {"error": "bytecode too short", "opcodes": [], "program_tokens": []}
    version = _u32(code, 0)
    is_pixel = (version >> 16) == 0xFFFF
    pos = 4  # skip the version token
    opcodes: List[int] = []
    program_tokens: List[int] = []
    comments = 0
    creator = ""
    has_ctab = False
    texture_samples = 0
    branches = 0
    loops = 0
    calls = 0
    mrt = 0
    end_seen = False
    while pos + 4 <= len(code):
        token = _u32(code, pos)
        op = token & 0xFFFF
        if op == END:
            end_seen = True
            pos += 4
            break
        if op == COMMENT:
            # The comment length is in the token's bits 16..30, in DWORDs;
            # the comment body follows immediately (no separate size DWORD).
            length_dwords = (token >> 16) & 0x7FFF
            body = code[pos + 4 : pos + 4 + 4 * length_dwords]
            if len(body) != 4 * length_dwords:
                return {"error": "truncated comment", "opcodes": opcodes,
                        "program_tokens": program_tokens}
            comments += 1
            if body[:4] == b"CTAB":
                has_ctab = True
            elif not creator and b"HLSL Shader Compiler" in body:
                creator = body.split(b"\x00", 1)[0].decode("latin-1", "replace")
            pos += 4 + 4 * length_dwords
            continue
        extra = (token >> 24) & 0xF
        instruction_size = 4 + 4 * extra
        if pos + instruction_size > len(code):
            return {"error": "instruction runs past bytecode", "opcodes": opcodes,
                    "program_tokens": program_tokens}
        opcodes.append(op)
        program_tokens.append(token)
        if op in TEXTURE_SAMPLE_OPS:
            texture_samples += 1
        if op in BRANCH_OPS:
            branches += 1
        if op in LOOP_OPS:
            loops += 1
        if op in CALL_OPS:
            calls += 1
        # Destination-register type decoding.  D3D9 splits the register type
        # across bits 28..30 and bits 11..12 (COLOROUT/DEPTHOUT live in the
        # second field).  MRT means a pixel shader writes an output colour
        # register beyond oC0 (COLOROUT, register type 8, index >= 1).  Vertex
        # output registers use type 6 and are not render targets.
        if extra >= 1 and op not in (0x1F, 0x51, 0x30, 0x2F):
            dst = _u32(code, pos + 4)
            reg_type = ((dst >> 28) & 0x7) | ((dst & 0x1800) >> 8)
            reg_num = dst & 0x7FF
            if is_pixel and reg_type == 8 and reg_num >= 1:
                mrt += 1
        pos += instruction_size
    if not end_seen:
        # Programs without END are malformed, but keep whatever was decoded.
        return {"error": "missing END token", "opcodes": opcodes,
                "program_tokens": program_tokens}
    return {
        "opcodes": opcodes,
        "program_tokens": program_tokens,
        "comments": comments,
        "creator": creator,
        "has_ctab": has_ctab,
        "texture_samples": texture_samples,
        "branches": branches,
        "loops": loops,
        "calls": calls,
        "mrt_writes": mrt,
        "error": None,
    }


def parse_fxp(name: str, data: bytes) -> List[ShaderChunk]:
    """Find and parse every validated BSVS/BSPS chunk in one .fxp."""
    chunks: List[ShaderChunk] = []
    pos = 0
    while pos < len(data):
        candidates = [(data.find(tag, pos), tag) for tag in CHUNK_TAGS]
        candidates = [(p, tag) for p, tag in candidates if p >= 0]
        if not candidates:
            break
        start, tag = min(candidates)
        if start + 12 > len(data):
            break
        meta = _u32(data, start + 4)
        size = _u32(data, start + 8)
        if size < 8 or start + 12 + size > len(data):
            pos = start + 4
            continue
        token = _u32(data, start + 12)
        if (token >> 16) not in (0xFFFE, 0xFFFF):
            pos = start + 4
            continue
        code = data[start + 12 : start + 12 + size]
        parsed = _parse_bytecode(code)
        counts = Counter(parsed.get("opcodes", []))
        err = parsed.get("error")
        if err and not parsed.get("opcodes"):
            pos = start + 4
            continue
        version = version_name(token)
        program_tokens = parsed.get("program_tokens", [])
        program_sha1 = hashlib.sha1(
            b"".join(struct.pack("<I", token) for token in program_tokens)
        ).hexdigest()
        chunks.append(
            ShaderChunk(
                source=name,
                offset=start,
                kind=CHUNK_TAGS[tag],
                meta=meta,
                size=size,
                version_token=token,
                version_name=version,
                opcodes=parsed.get("opcodes", []),
                opcode_counts=dict(counts),
                instruction_count=len(parsed.get("opcodes", [])),
                comment_count=parsed.get("comments", 0),
                creator=parsed.get("creator", ""),
                has_ctab=bool(parsed.get("has_ctab")),
                texture_samples=parsed.get("texture_samples", 0),
                branch_count=parsed.get("branches", 0),
                loop_count=parsed.get("loops", 0),
                call_count=parsed.get("calls", 0),
                mrt_writes=parsed.get("mrt_writes", 0),
                bytecode_sha1=hashlib.sha1(code).hexdigest(),
                program_sha1=program_sha1,
                error=err,
            )
        )
        pos = start + 12 + size
    return chunks


SOURCE_EXTENSIONS = (".hlsl", ".fx", ".fxh", ".vsh", ".psh", ".vs", ".ps", ".glsl", ".cg")
SOURCE_MARKERS = (b"float4 ", b"technique", b"sampler2D", b"technique10", b"SamplerState")


@dataclass
class ShaderInventory:
    files: List[dict]
    chunks: List[ShaderChunk]
    extensions: Dict[str, int]
    directories: Dict[str, int]
    source_files: List[dict]
    source_marker_files: List[str]
    total_bytes: int

    def summary(self) -> dict:
        opcode_hist: Counter = Counter()
        for chunk in self.chunks:
            opcode_hist.update(chunk.opcode_counts)
        versions = Counter(chunk.version_name for chunk in self.chunks)
        kinds = Counter(chunk.kind for chunk in self.chunks)
        unique_bytecode = {chunk.bytecode_sha1 for chunk in self.chunks}
        unique_programs = {(chunk.version_token, chunk.program_sha1) for chunk in self.chunks}
        exact = [c for c in self.chunks if c.uses_port_subset]
        loose = [c for c in self.chunks if c.uses_port_subset_loose]
        return {
            "file_count": len(self.files),
            "total_bytes": self.total_bytes,
            "extensions": dict(sorted(self.extensions.items())),
            "directories": dict(sorted(self.directories.items())),
            "source_files": len(self.source_files),
            "source_marker_files": self.source_marker_files,
            "chunk_count": len(self.chunks),
            "chunk_kinds": dict(kinds),
            "versions": dict(versions),
            "unique_bytecode": len(unique_bytecode),
            "unique_programs": len(unique_programs),
            "port_subset_exact": len(exact),
            "port_subset_exact_fraction": round(len(exact) / len(self.chunks), 4)
            if self.chunks else 0.0,
            "port_subset_loose": len(loose),
            "port_subset_loose_fraction": round(len(loose) / len(self.chunks), 4)
            if self.chunks else 0.0,
            "opcode_histogram": {
                OPCODES.get(op, "op_0x%02x" % op): count
                for op, count in opcode_hist.most_common()
            },
            "texture_samples": sum(c.texture_samples for c in self.chunks),
            "branch_chunks": sum(1 for c in self.chunks if c.branch_count),
            "loop_chunks": sum(1 for c in self.chunks if c.loop_count),
            "mrt_chunks": sum(1 for c in self.chunks if c.mrt_writes),
            "parse_errors": sum(1 for c in self.chunks if c.error),
        }

    def rows(self) -> List[dict]:
        return [chunk.as_dict() for chunk in self.chunks]


# ---------------------------------------------------------------------------
# Whole-package parsing (container + per-chunk metadata trailers)
# ---------------------------------------------------------------------------
#
# Three container layouts ship in ``Shaders.bsa`` / ``Update.bsa``:
#
# * ``BSFX`` (120 lighting packages): ``b"BSFX" u32 0 u32 chunks`` followed by
#   ``chunks`` records.
# * ``u32 2 u32 2`` (``shaders001``/``shaders002``): an 8-byte header, then
#   records.
# * no header (``imagespace002``): the file starts with the first ``BSVS``.
#
# A record is ``b"BSVS"|b"BSPS" u32 meta u32 size bytecode[size] trailer``.
# The trailer is 36 bytes after a vertex chunk and 78 bytes after a pixel
# chunk; seven pixel chunks carry 8 extra bytes of embedded compiler source
# text (86-byte trailer).  All of that was measured across the 10,475 chunks:
# the gap from the end of one bytecode blob to the next chunk tag is exactly
# 36 / 78 / 86 bytes, and the file ends after the last trailer.  The trailer is
# reflection state (register binding slots padded with 0xab and, in a few
# cases, an 8-16 byte source snippet); it is not needed to translate code.

PACKAGE_MAGIC = b"BSFX"


def package_header(data: bytes) -> dict:
    """Describe the container header without decoding any chunk."""
    if data[:4] == PACKAGE_MAGIC:
        if len(data) < 12:
            return {"container": "BSFX", "error": "truncated BSFX header"}
        _zero, count = struct.unpack_from("<II", data, 4)
        return {"container": "BSFX", "header_bytes": 12, "declared_chunks": count}
    if len(data) >= 8:
        a, b = struct.unpack_from("<II", data, 0)
        if a == 2 and b == 2:
            return {"container": "8byte", "header_bytes": 8, "declared_chunks": None}
    return {"container": "raw", "header_bytes": 0, "declared_chunks": None}


@dataclass
class PackageChunk:
    source: str
    kind: str                 # "vs" or "ps"
    offset: int
    meta: int
    size: int
    code: bytes
    trailer: bytes
    version_token: int
    version_name: str
    trailer_size: int

    @property
    def key(self) -> int:
        """The engine's permutation key: the package file stem and chunk meta."""
        return self.meta


@dataclass
class ShaderPackage:
    path: str
    size: int
    container: str
    header_bytes: int
    chunks: List[PackageChunk]

    @property
    def name(self) -> str:
        base = self.path.rsplit("\\", 1)[-1]
        return base[:-4] if base.lower().endswith(".fxp") else base

    @property
    def stem_flags(self) -> Optional[int]:
        """Lighting package stems are ``p<hex flags>``; return the flags."""
        name = self.name
        if self.path.lower().startswith("shadersfx\\lighting\\") and len(name) > 1 and name[0] in "pP":
            try:
                return int(name[1:], 16)
            except ValueError:
                return None
        return None

    @property
    def family_path(self) -> str:
        lowered = self.path.lower()
        if "imagespace" in lowered:
            return "imagespace"
        if "lighting" in lowered:
            return "lighting"
        if "shaders00" in lowered:
            return "shaders"
        return "other"

    def chunk_kinds(self) -> Counter:
        return Counter(chunk.kind for chunk in self.chunks)


def _next_chunk_tag(data: bytes, start: int) -> int:
    best = len(data)
    for tag in CHUNK_TAGS:
        found = data.find(tag, start)
        if found >= 0 and found < best:
            best = found
    return best


def parse_package(name: str, data: bytes) -> ShaderPackage:
    """Parse one ``.fxp`` into records with their metadata trailers."""
    header = package_header(data)
    pos = header["header_bytes"]
    chunks: List[PackageChunk] = []
    while pos + 12 <= len(data):
        tag = data[pos:pos + 4]
        if tag not in CHUNK_TAGS:
            # Raw/8-byte layouts can carry a few bytes we do not model; fall
            # back to the tag scan rather than guessing a length.
            nxt = _next_chunk_tag(data, pos + 1)
            if nxt >= len(data):
                break
            pos = nxt
            continue
        meta, size = struct.unpack_from("<II", data, pos + 4)
        if size < 8 or pos + 12 + size > len(data):
            break
        code = data[pos + 12:pos + 12 + size]
        token = struct.unpack_from("<I", code, 0)[0]
        if (token >> 16) not in (0xFFFE, 0xFFFF):
            pos += 4
            continue
        nxt = _next_chunk_tag(data, pos + 12 + size)
        trailer = data[pos + 12 + size:nxt]
        chunks.append(PackageChunk(
            source=name,
            kind=CHUNK_TAGS[tag],
            offset=pos,
            meta=meta,
            size=size,
            code=code,
            trailer=trailer,
            version_token=token,
            version_name=version_name(token),
            trailer_size=len(trailer),
        ))
        pos = nxt
    return ShaderPackage(name, len(data), header["container"],
                         header["header_bytes"], chunks)


def package_overrides(packages: Iterable[ShaderPackage]) -> Dict[str, ShaderPackage]:
    """Collapse archive order into the last package seen for each path."""
    out: Dict[str, ShaderPackage] = {}
    for package in packages:
        out[package.path.lower()] = package
    return out


def inventory_archive(archive) -> ShaderInventory:
    """Walk a BSA of shader files (e.g. Shaders.bsa) and inventory them."""
    files: List[dict] = []
    chunks: List[ShaderChunk] = []
    extensions: Counter = Counter()
    directories: Counter = Counter()
    source_files: List[dict] = []
    source_marker_files: List[str] = []
    total_bytes = 0
    for entry in archive.list_files():
        data = archive.read_file(entry.path)
        files.append({"path": entry.path, "size": len(data)})
        total_bytes += len(data)
        ext = entry.extension
        extensions[ext] += 1
        directory = entry.path.rsplit("\\", 1)[0] if "\\" in entry.path else ""
        directories[directory] += 1
        lowered = entry.path.lower()
        if lowered.endswith(SOURCE_EXTENSIONS):
            source_files.append({"path": entry.path, "size": len(data)})
        if any(marker in data for marker in SOURCE_MARKERS):
            source_marker_files.append(entry.path)
        if ext == "fxp":
            chunks.extend(parse_fxp(entry.path, data))
    return ShaderInventory(
        files=sorted(files, key=lambda item: item["path"].lower()),
        chunks=chunks,
        extensions=dict(extensions),
        directories=dict(directories),
        source_files=source_files,
        source_marker_files=sorted(source_marker_files),
        total_bytes=total_bytes,
    )
