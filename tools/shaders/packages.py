"""Shader-family classification, deduplication and main-menu subset.

Inputs come from the retail archives but nothing here stores game bytes.  The
public functions consume ``ShaderPackage`` objects (``tools/skyrim/shaders.py``)
and return plain dictionaries that the CLI in ``tools/shaders/report.py``
writes to ``E:\\Skyrim_N3DS_build\\assets\\shaders``.

Family model
------------
Skyrim LE does not ship source, so technique names cannot be read out of the
files.  Classification therefore uses three independent signals:

* package path (``shadersfx\\imagespaceNNN.fxp``, ``shadersfx\\shadersNNN.fxp``,
  ``shadersfx\\lighting\\059\\p<hex>.fxp``),
* the CTAB constant vocabulary (``WaterParams``, ``MembraneVars``,
  ``MultiTextureDiffuse0Sampler``, ``FlareColor``, ...),
* opcode/declaration features (texture-sample count, branch/loop, VS input
  usages such as ``blendweight``).

Every rule is written down in ``FAMILY_RULES`` and is deterministic.
"""

from __future__ import annotations

import hashlib
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

try:  # normal use: PYTHONPATH=tools
    from skyrim import bsa, shaders
except ImportError:  # running from the repository root
    from tools.skyrim import bsa, shaders  # type: ignore

from . import asm

FAMILIES = (
    "ui/interface",
    "sky",
    "water",
    "terrain/landscape",
    "lighting/deferred/shadows",
    "skinning/characters",
    "particles/effects",
    "grass/trees",
    "post-process/imagespace",
    "blit/copy",
)

# ---------------------------------------------------------------------------
# Vocabulary tables (derived from the shipped CTABs, measured 2026-11-24)
# ---------------------------------------------------------------------------

WATER_NAMES = {
    "WaterParams", "ReflectionColor", "ShallowColor", "DeepColor",
    "Normals01Sampler", "Normals02Sampler", "Normals03Sampler",
    "NormalsAmplitude", "RefractionSampler", "RefractionPower",
    "WadingVars", "RainVars", "FresnelRI", "PosAdjust", "VarAmounts",
    "DepthControl", "ReflectPlane", "TextureProj", "ProjData",
    "CubeMapSampler", "SunColor", "SunDir", "FogNearColor", "FogParam",
    "SplitDistances", "HeightMap", "HeightMap01", "HeightMap02",
    "DisplaceMap", "DisplacementSampler", "fAmplitude", "fTexScale",
    "fTexScroll0", "fTexScroll1", "fTexScroll2", "fNoiseScale",
}
SKY_NAMES = {"PParams", "ColorRange", "SunColor", "SunDir", "SunDisc", "Atmosphere"}
TERRAIN_NAMES = {
    "MultiTextureDiffuse0Sampler", "MultiTextureDiffuse1Sampler",
    "MultiTextureDiffuse2Sampler", "MultiTextureDiffuse3Sampler",
    "MultiTextureDiffuse4Sampler", "MultiTextureDiffuse5Sampler",
    "MultiTextureNormal0Sampler", "MultiTextureNormal1Sampler",
    "MultiTextureNormal2Sampler", "MultiTextureNormal3Sampler",
    "MultiTextureNormal4Sampler", "MultiTextureNormal5Sampler",
    "MultiTextureDiffuseLODSampler", "MultiTextureNormalLODSampler",
    "LODNoiseSampler", "LODTexParams", "HeightSampler",
    "WorldMapOverlayNormalSampler", "WorldMapOverlayNormalSnowSampler",
    "WorldMapOverlayParameters",
}
CHARACTER_NAMES = {
    "MembraneRimColor", "MembraneVars", "Facegen0Sampler", "Facegen1Sampler",
    "TintMask0", "TintMask1", "TintMask2", "TintMask3", "TintMask4",
    "TintMask5", "TintMask6", "TintMask7", "TintMask8", "TintMask9",
    "TintMask10", "TintMask11", "TintMask12", "TintMask13", "TintMask14",
    "TintMask15", "SkinTintColor", "HairTintColor", "TintColor",
}
EFFECT_NAMES = {
    "FlareColor", "FlareHDR", "AmbientColor", "DiffuseColor", "Diffuse",
    "BlendSampler", "DebugColor", "SparkleSampler", "SparkleParams",
    "Alpha", "BlendAmount", "ColorSelect", "Fade", "Tint",
    "BloodColor", "BloodAlpha",
}
LIGHTING_NAMES = {
    "DirLightColor", "DirLightDirection", "DirectionalAmbient", "EmitColor",
    "FogColor", "MaterialData", "NormalSampler", "DiffuseSampler",
    "PointLightColor", "PointLightPosition", "ShadowMaskSampler",
    "ShadowLightMaskSelect", "SpecularColor", "SpecularSampler",
    "LightingEffectParams", "SubSurfaceSampler", "BackLightMaskSampler",
    "EnvSampler", "EnvMaskSampler", "EnvmapData", "ProjectedNoiseSampler",
    "ProjectedUVParams", "ProjectedUVParams2", "MultiLayerParallaxSampler",
    "MultiLayerParallaxData", "VPOSOffset",
}

#: Family rule order.  The first matching rule wins; ``lighting`` is the
#: fallback for lit material shaders.  The rules are exported so the report and
#: docs can quote them verbatim.
FAMILY_RULES = (
    ("post-process/imagespace", "package path contains 'imagespace'"),
    ("water", "PS/VS uses any WaterParams / reflection / refraction CTAB name"),
    ("sky", "PS/VS uses PParams or a sky colour range and is not water"),
    ("terrain/landscape", "PS/VS uses MultiTexture* / LODNoise / WorldMapOverlay*"),
    ("skinning/characters", "VS declares blendweight+blendindices, or PS uses membrane/facegen/tint-mask CTAB names"),
    ("particles/effects", "PS uses flare/blood/diffuse/alpha effect names, or VS is colour+texcoord billboard"),
    ("blit/copy", "PS has <=3 instructions and <=1 texld and no lighting CTAB names"),
    ("ui/interface", "no lighting CTAB names and not one of the above"),
    ("lighting/deferred/shadows", "all remaining lit material shaders"),
    ("grass/trees", "no shipped package; grass/tree materials ride lighting/059 (reported as 0)"),
)


@dataclass
class ChunkRecord:
    package: shaders.ShaderPackage
    chunk: shaders.PackageChunk
    program: asm.Program
    features: dict
    constants: Set[str]
    used_constants: Set[str]
    family: str
    bytecode_sha1: str
    program_sha1: str
    opcode_sha1: str
    technique: str

    @property
    def is_pixel(self) -> bool:
        return self.chunk.kind == "ps"


def _program_hash(code: bytes) -> str:
    """SHA-1 over instruction tokens only (comments/CTAB removed)."""
    digest = hashlib.sha1()
    pos = 4
    digest.update(code[:4])
    while pos + 4 <= len(code):
        token = int.from_bytes(code[pos:pos + 4], "little")
        opcode = token & 0xFFFF
        if opcode == 0xFFFF:
            digest.update(token.to_bytes(4, "little"))
            break
        if opcode == 0xFFFE:
            pos += 4 + 4 * ((token >> 16) & 0x7FFF)
            continue
        words = 1 + ((token >> 24) & 0xF)
        digest.update(code[pos:pos + 4 * words])
        pos += 4 * words
    return digest.hexdigest()


def _opcode_hash(program: asm.Program) -> str:
    """SHA-1 over the opcode sequence (the existing 6,455-program metric)."""
    digest = hashlib.sha1()
    digest.update(program.version.to_bytes(4, "little"))
    for ins in program.instructions:
        digest.update(ins.opcode.to_bytes(4, "little"))
    return digest.hexdigest()


def _declarations(features: dict) -> Set[str]:
    return {usage for usage, _index, _reg in features["inputs"]}


def classify(chunk: shaders.PackageChunk, program: asm.Program,
             features: dict) -> str:
    names = set(features["constant_names"])
    used = set(_used_names(program))
    path = chunk.source.lower()

    if "imagespace" in path:
        return "post-process/imagespace"

    if chunk.kind == "vs":
        usages = _declarations(features)
        if {"blendweight", "blendindices"} <= usages:
            return "skinning/characters"
        if "color" in usages and not ({"normal", "tangent"} & usages):
            return "particles/effects"
        if "texcoord" in usages and "color" not in usages and "normal" not in usages:
            return "ui/interface"
        return "lighting/deferred/shadows"

    # Pixel shaders: CTAB vocabulary is the strongest signal.
    if names & WATER_NAMES:
        return "water"
    if names & SKY_NAMES:
        return "sky"
    if names & TERRAIN_NAMES:
        return "terrain/landscape"

    if names & CHARACTER_NAMES:
        return "skinning/characters"
    if names & EFFECT_NAMES:
        return "particles/effects"

    texld = features["texture_samples"]
    if texld <= 1 and features["instructions"] <= 3 and not (names & LIGHTING_NAMES):
        return "blit/copy"
    if not (names & LIGHTING_NAMES):
        return "ui/interface"
    return "lighting/deferred/shadows"


def _used_names(program: asm.Program) -> List[str]:
    mapping = asm.constant_name_map(program)
    names = []
    for ins in program.instructions:
        for src in ins.srcs:
            if src.reg_type == asm.REG_CONST:
                name = mapping.get((2, src.index))
                if name:
                    names.append(name)
    return names


def load_packages(data_dir: str = r"E:\SteamLibrary\steamapps\common\Skyrim\Data",
                  include_update: bool = True) -> List[shaders.ShaderPackage]:
    """Read every ``.fxp`` from the read-only install, Update.bsa last."""
    import os

    packages: List[shaders.ShaderPackage] = []
    names = ["Skyrim - Shaders.bsa"]
    if include_update:
        names.append("Update.bsa")
    for archive_name in names:
        path = os.path.join(data_dir, archive_name)
        if not os.path.exists(path):
            continue
        archive = bsa.BsaArchive(path)
        for entry in sorted(archive.list_files(), key=lambda item: item.path.lower()):
            if not entry.path.lower().endswith(".fxp"):
                continue
            data = archive.read_file(entry.path)
            package = shaders.parse_package(entry.path, data)
            package.origin = archive_name  # dynamic tag, not part of the format
            packages.append(package)
    return packages


def effective_packages(packages: Sequence[shaders.ShaderPackage]) -> List[shaders.ShaderPackage]:
    """Apply archive override order (Update.bsa replaces same-path files)."""
    out: Dict[str, shaders.ShaderPackage] = {}
    for package in packages:
        out[package.path.lower()] = package
    return [out[key] for key in sorted(out)]


def build_records(packages: Sequence[shaders.ShaderPackage]) -> List[ChunkRecord]:
    records: List[ChunkRecord] = []
    for package in packages:
        for chunk in package.chunks:
            program = asm.parse_program(chunk.code)
            features = asm.feature_vector(program)
            constants = set(features["constant_names"])
            used = set(_used_names(program))
            records.append(ChunkRecord(
                package=package,
                chunk=chunk,
                program=program,
                features=features,
                constants=constants,
                used_constants=used,
                family=classify(chunk, program, features),
                bytecode_sha1=hashlib.sha1(chunk.code).hexdigest(),
                program_sha1=_program_hash(chunk.code),
                opcode_sha1=_opcode_hash(program),
                technique=package.name,
            ))
    return records


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------

def dedup_summary(records: Sequence[ChunkRecord]) -> dict:
    bytecodes = {r.bytecode_sha1 for r in records}
    programs = {(r.chunk.version_token, r.program_sha1) for r in records}
    opcodes = {(r.chunk.version_token, r.opcode_sha1) for r in records}
    packages = {r.package.path.lower() for r in records}
    techniques = {(r.package.path.lower(), r.technique) for r in records}
    return {
        "chunks": len(records),
        "distinct_bytecode": len(bytecodes),
        "distinct_instruction_streams": len(programs),
        "distinct_opcode_sequences": len(opcodes),
        "packages": len(packages),
        "package_technique_pairs": len(techniques),
        "chunks_per_distinct_bytecode": round(len(records) / len(bytecodes), 4) if bytecodes else 0.0,
        "chunks_per_distinct_opcode_sequence": round(len(records) / len(opcodes), 4) if opcodes else 0.0,
    }


def family_summary(records: Sequence[ChunkRecord]) -> dict:
    out: Dict[str, dict] = {}
    for family in FAMILIES:
        subset = [r for r in records if r.family == family]
        packages = sorted({r.package.name for r in subset})
        bytecodes = {r.bytecode_sha1 for r in subset}
        programs = {(r.chunk.version_token, r.program_sha1) for r in subset}
        opcodes = {(r.chunk.version_token, r.opcode_sha1) for r in subset}
        ps = [r for r in subset if r.chunk.kind == "ps"]
        vs = [r for r in subset if r.chunk.kind == "vs"]
        opcode_hist: Counter = Counter()
        for record in subset:
            opcode_hist.update(record.features["opcodes"])
        out[family] = {
            "chunks": len(subset),
            "pixel_chunks": len(ps),
            "vertex_chunks": len(vs),
            "distinct_bytecode": len(bytecodes),
            "distinct_instruction_streams": len(programs),
            "distinct_opcode_sequences": len(opcodes),
            "packages": len(packages),
            "package_names": packages,
            "bytes": sum(r.chunk.size for r in subset),
            "texture_samples": sum(r.features["texture_samples"] for r in subset),
            "branch_chunks": sum(1 for r in subset if r.features["branches"]),
            "loop_chunks": sum(1 for r in subset if r.features["loops"]),
            "mrt_chunks": sum(1 for r in subset if r.features["mrt"]),
            "mean_instructions": round(sum(r.features["instructions"] for r in subset) / len(subset), 2) if subset else 0.0,
            "max_instructions": max((r.features["instructions"] for r in subset), default=0),
            "texld_histogram": dict(sorted(Counter(r.features["texture_samples"] for r in subset).items())),
            "top_opcodes": {asm.OPCODE_NAMES.get(op, "op_0x%02x" % op): count
                            for op, count in opcode_hist.most_common(12)},
            "constant_vocabulary": sorted({name for r in subset for name in r.constants}),
        }
    return out


def package_summary(records: Sequence[ChunkRecord]) -> List[dict]:
    rows: List[dict] = []
    by_package: Dict[str, List[ChunkRecord]] = defaultdict(list)
    for record in records:
        by_package[record.package.path].append(record)
    for path in sorted(by_package):
        subset = by_package[path]
        package = subset[0].package
        rows.append({
            "path": path,
            "name": package.name,
            "container": package.container,
            "file_bytes": package.size,
            "chunks": len(subset),
            "vs": sum(1 for r in subset if r.chunk.kind == "vs"),
            "ps": sum(1 for r in subset if r.chunk.kind == "ps"),
            "distinct_bytecode": len({r.bytecode_sha1 for r in subset}),
            "distinct_programs": len({(r.chunk.version_token, r.program_sha1) for r in subset}),
            "family": Counter(r.family for r in subset).most_common(1)[0][0],
            "stem_flags": package.stem_flags,
            "texture_samples": sum(r.features["texture_samples"] for r in subset),
            "branch_chunks": sum(1 for r in subset if r.features["branches"]),
            "constants": sorted({name for r in subset for name in r.constants}),
            "chunk_sizes": sorted({r.chunk.size for r in subset}),
        })
    return rows


# ---------------------------------------------------------------------------
# TEV feasibility helpers
# ---------------------------------------------------------------------------

def tev_estimate(record: ChunkRecord) -> dict:
    """A conservative estimate of how many PICA TEV stages a PS needs.

    This is a *feasibility screen*, not the translator: it counts arithmetic
    instructions in the final expression when the code is a straight line.
    The real translator in ``source/d3d9_tev.c`` is authoritative and is
    exercised by ``tests/tev_*``.
    """
    program = record.program
    features = record.features
    alu = 0
    consts = set()
    for ins in program.instructions:
        if ins.opcode in (0x1F, 0x51, 0x42):
            continue
        alu += 1
        for src in ins.srcs:
            if src.reg_type == asm.REG_CONST:
                consts.add(src.index)
    unsupported = sorted({
        ins.name for ins in program.instructions
        if ins.opcode not in asm.TEV_SIMPLE_OPCODES
    })
    return {
        "bytecode_ok": features["error"] is None,
        "opcode_subset": features["simple_opcodes"],
        "unsupported_opcodes": unsupported,
        "alu_instructions": alu,
        "stage_estimate_min": max(0, alu - features["texture_samples"]),
        "constants_used": len(consts),
        "textures_used": features["texture_samples"],
        "branches": features["branches"],
        "loops": features["loops"],
        "fits_6_stages": alu - features["texture_samples"] <= 6,
        "fits_3_textures": features["texture_samples"] <= 3,
    }


def feasibility_table(records: Sequence[ChunkRecord]) -> dict:
    """Conservative Python screen, not the C translator.

    The authoritative acceptance numbers come from running
    ``tests/tev_real_smoke.c`` over the extracted chunks; this table only
    estimates which pixel shaders are candidates (opcode subset, <=3 textures,
    <=6 stages).
    """
    out: Dict[str, dict] = {}
    for family in FAMILIES:
        subset = [r for r in records if r.family == family and r.chunk.kind == "ps"]
        if not subset:
            out[family] = {"pixel_chunks": 0, "translator_accepts": 0,
                           "translator_fraction": 0.0, "reasons": {}}
            continue
        reasons: Counter = Counter()
        accepts = 0
        for record in subset:
            estimate = tev_estimate(record)
            if (estimate["bytecode_ok"] and estimate["opcode_subset"]
                    and estimate["fits_3_textures"] and estimate["fits_6_stages"]):
                accepts += 1
            else:
                if not estimate["bytecode_ok"]:
                    reasons["bytecode error: " + str(record.features["error"])] += 1
                for opcode in estimate["unsupported_opcodes"]:
                    reasons["unsupported opcode: " + opcode] += 1
                if not estimate["fits_3_textures"]:
                    reasons["more than 3 texture stages"] += 1
                if not estimate["fits_6_stages"]:
                    reasons["more than 6 TEV stages"] += 1
        out[family] = {
            "pixel_chunks": len(subset),
            "translator_accepts": accepts,
            "translator_fraction": round(accepts / len(subset), 4),
            "reasons": dict(reasons.most_common(12)),
        }
    return out


__all__ = [
    "FAMILIES", "FAMILY_RULES", "ChunkRecord", "load_packages",
    "effective_packages", "build_records", "dedup_summary", "family_summary",
    "package_summary", "tev_estimate", "feasibility_table", "classify",
    "WATER_NAMES", "SKY_NAMES", "TERRAIN_NAMES", "CHARACTER_NAMES",
    "EFFECT_NAMES", "LIGHTING_NAMES",
]
