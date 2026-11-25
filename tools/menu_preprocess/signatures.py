"""The two menu-light normal decode contracts.

The bytecode recognizer in ``source/d3d9_menu_light.c`` remains authoritative.
``probe_bytecode`` below is a deliberately partial host-side inventory probe: it
checks the version, instruction count, c8 literal, declarations and the normal
decode sequence, but not every tail instruction/operand.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

from .common import ContractError, DataError

try:  # PYTHONPATH=tools
    from shaders import asm
except ImportError:  # repository root
    from tools.shaders import asm  # type: ignore


@dataclass(frozen=True)
class Signature:
    name: str
    pipeline_id: str
    instructions: int
    bytecode_bytes: int
    c8: Tuple[float, float, float, float]
    formula: str
    source_channels: Tuple[str, str, str]

    def as_dict(self) -> dict:
        return {
            "name": self.name,
            "pipeline_id": self.pipeline_id,
            "instructions": self.instructions,
            "retail_bytecode_bytes": self.bytecode_bytes,
            "c8": list(self.c8),
            "formula": self.formula,
            "source_channels": {
                "x": self.source_channels[0],
                "y": self.source_channels[1],
                "z": self.source_channels[2],
            },
            "output_channels": {"x": "r", "y": "g", "z": "b", "a": "opaque_255"},
        }


SIGNATURES = {
    "p5_MAD_NORMAL": Signature(
        name="p5_MAD_NORMAL",
        pipeline_id="static.p5_mad_normal",
        instructions=24,
        bytecode_bytes=840,
        c8=(2.0, -1.0, 1.0, 0.0),
        formula="normal.xyz = normal.xzyw * 2 - 1",
        source_channels=("r", "a", "g"),
    ),
    "p1200005_ADD_NORMAL": Signature(
        name="p1200005_ADD_NORMAL",
        pipeline_id="static.p1200005_add_normal",
        instructions=25,
        bytecode_bytes=852,
        c8=(-0.5, 1.0, 0.0, 0.0),
        formula="normal.xyz = (normal.xzyw - 0.5) * 2",
        source_channels=("r", "a", "g"),
    ),
}


def normalize_signature(value: str) -> Signature:
    if not isinstance(value, str) or value not in SIGNATURES:
        raise ContractError(
            "signature must be one of %s"
            % ", ".join(SIGNATURES)
        )
    return SIGNATURES[value]


def _encode_signed(value: float) -> int:
    # D3D9 UNORM sampling is v/255.  Both retail formulas map the endpoints
    # exactly; round-to-nearest with ties upward makes the 8-bit result explicit.
    return max(0, min(255, int((value + 1.0) * 127.5 + 0.5)))


def decode_normal_components(signature: Signature, x: int, y: int,
                             z: int) -> Tuple[int, int, int, int]:
    """Decode already-selected X/Y/Z UNORM bytes to RGBA8 XYZ + opaque A."""

    if any(isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 255
           for v in (x, y, z)):
        raise DataError("normal component channels must be integers in [0, 255]")
    raw = (x / 255.0, y / 255.0, z / 255.0)
    if signature.name == "p5_MAD_NORMAL":
        decoded = tuple(v * 2.0 - 1.0 for v in raw)
    else:
        decoded = tuple((v - 0.5) * 2.0 for v in raw)
    return (_encode_signed(decoded[0]), _encode_signed(decoded[1]),
            _encode_signed(decoded[2]), 255)


def decode_normal_byte(signature: Signature, r: int, g: int, b: int,
                       a: int) -> Tuple[int, int, int, int]:
    """Decode one RGBA8 source texel to linear XYZ + opaque alpha.

    ``b`` is intentionally unused: the retail ``xzyw`` swizzle puts source G in
    Z and source A in Y.  This is why a conventional RGBA normal map cannot be
    passed through silently.
    """

    if any(isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 255
           for v in (r, g, b, a)):
        raise DataError("normal texel channels must be integers in [0, 255]")
    return decode_normal_components(signature, r, a, g)


def decode_linear_byte(signature: Signature, r: int, g: int,
                       a: int) -> Tuple[float, float, float]:
    if any(isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 255
           for v in (r, g, a)):
        raise DataError("normal texel channels must be integers in [0, 255]")
    raw = (r / 255.0, a / 255.0, g / 255.0)
    if signature.name == "p5_MAD_NORMAL":
        return tuple(v * 2.0 - 1.0 for v in raw)  # type: ignore[return-value]
    return tuple((v - 0.5) * 2.0 for v in raw)  # type: ignore[return-value]


def _is_const(ins: asm.Instruction, index: int) -> bool:
    return (ins.dst is not None and ins.dst.reg_type == asm.REG_CONST
            and ins.dst.index == index)


def _is_src(ins: asm.Instruction, slot: int, reg_type: int, index: int,
            swizzle: Optional[str] = None, modifier: int = 0) -> bool:
    if slot >= len(ins.srcs):
        return False
    src = ins.srcs[slot]
    if swizzle is not None and len(swizzle) == 1:
        swizzle = swizzle * 4
    return (src.reg_type == reg_type and src.index == index
            and (swizzle is None or src.swizzle == swizzle)
            and src.modifier == modifier and not src.relative)


def probe_bytecode(code: bytes, *, require_ctab: bool = False) -> dict:
    """Partial structural probe used by read-only inventory sampling.

    It never accepts a shader for runtime use.  A successful probe says that the
    normal-decode portion and c8 literal agree with a retail signature; callers
    must still use ``d9_menu_light_match`` for full acceptance.
    """

    if len(code) > 64 * 1024 * 1024:
        raise DataError("bytecode exceeds the host probe input limit")
    program = asm.parse_program(code)
    if program.error:
        raise DataError("bytecode probe: %s" % program.error)
    if program.version != 0xFFFF0300 or program.vertex:
        raise DataError("bytecode probe: expected a ps_3_0 pixel program")

    definitions = [ins for ins in program.instructions
                   if ins.opcode == 0x51 and _is_const(ins, 8)]
    if len(definitions) != 1 or definitions[0].dst is None:
        raise DataError("bytecode probe: expected exactly one def c8")
    if definitions[0].dst.mask != 0xF or definitions[0].dst.modifier != 0:
        raise DataError("bytecode probe: def c8 mask/modifier mismatch")
    c8 = tuple(float(v) for v in definitions[0].literal)

    normal_sample = None
    for index, ins in enumerate(program.instructions):
        if (ins.opcode == 0x42 and ins.dst is not None
                and ins.dst.reg_type == asm.REG_TEMP and ins.dst.index == 1
                and _is_src(ins, 0, asm.REG_INPUT, 0, "xyzw")
                and _is_src(ins, 1, asm.REG_SAMPLER, 1, "xyzw")):
            normal_sample = index
            break
    if normal_sample is None:
        raise DataError("bytecode probe: normal texld r1,v0,s1 not found")
    if normal_sample + 1 >= len(program.instructions):
        raise DataError("bytecode probe: normal decode instruction is missing")

    matched = None
    for name, signature in SIGNATURES.items():
        if len(program.instructions) != signature.instructions:
            continue
        if c8 != signature.c8:
            continue
        first = program.instructions[normal_sample + 1]
        if name == "p5_MAD_NORMAL":
            ok = (first.opcode == 0x04 and first.dst is not None
                  and first.dst.reg_type == asm.REG_TEMP
                  and first.dst.index == 1 and first.dst.mask == 0x7
                  and first.dst.modifier == 0
                  and _is_src(first, 0, asm.REG_TEMP, 1, "xzyw")
                  and _is_src(first, 1, asm.REG_CONST, 8, "x")
                  and _is_src(first, 2, asm.REG_CONST, 8, "y"))
        else:
            ok = (first.opcode == 0x02 and first.dst is not None
                  and first.dst.reg_type == asm.REG_TEMP
                  and first.dst.index == 1 and first.dst.mask == 0x7
                  and first.dst.modifier == 0
                  and _is_src(first, 0, asm.REG_TEMP, 1, "xzyw")
                  and _is_src(first, 1, asm.REG_CONST, 8, "x"))
        if ok:
            matched = signature
            break
    if matched is None:
        raise DataError(
            "bytecode probe: normal decode is neither accepted p5 MAD nor "
            "p1200005 ADD signature"
        )

    declarations = {(d.reg_type, d.reg_index, d.usage, d.usage_index,
                     d.texture_type) for d in program.declarations}
    expected_declarations = {
        (asm.REG_INPUT, 0, 5, 0, 0),
        (asm.REG_INPUT, 1, 10, 0, 0),
        (asm.REG_INPUT, 2, 10, 1, 0),
        (asm.REG_SAMPLER, 0, 0, 0, 2),
        (asm.REG_SAMPLER, 1, 0, 0, 2),
    }
    if not expected_declarations <= declarations:
        raise DataError("bytecode probe: required texcoord/colour/sampler dcl missing")

    constants = {
        c.name: (c.register_set, c.register_index)
        for c in program.constants
    }
    expected_names = {
        "DiffuseSampler", "NormalSampler", "DirLightColor",
        "DirLightDirection", "DirectionalAmbient", "EmitColor", "FogColor",
        "MaterialData",
    }
    missing_names = sorted(expected_names - set(constants))
    if require_ctab and missing_names:
        raise DataError("bytecode probe: CTAB is missing %s"
                        % ", ".join(missing_names))

    return {
        "signature": matched.name,
        "instructions": len(program.instructions),
        "bytecode_bytes": len(code),
        "c8": list(c8),
        "formula": matched.formula,
        "declarations_ok": True,
        "ctab": {
            "required_names_present": not missing_names,
            "missing_names": missing_names,
            "bindings": {name: {"set": constants[name][0],
                                "index": constants[name][1]}
                         for name in sorted(expected_names & set(constants))},
            "directional_ambient_note": (
                "CTAB binds DirectionalAmbient to c0, but this specialised "
                "program consumes c0 as a tangent-basis row; the name is "
                "vocabulary only for the preprocessed path."
            ),
        },
        "probe_scope": (
            "partial host inventory probe; source/d3d9_menu_light.c remains "
            "the authoritative full structural matcher"
        ),
    }


__all__ = [
    "Signature", "SIGNATURES", "normalize_signature", "decode_normal_components",
    "decode_normal_byte", "decode_linear_byte", "probe_bytecode",
]
