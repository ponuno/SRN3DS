"""Minimal D3D9 SM3 bytecode decoder used by the shader-family reports.

This is the Python twin of the C translator in ``source/d3d9_tev.c``.  It is
deliberately small: it decodes the token stream, the CTAB constant table and
the ``dcl`` declarations, and extracts the features the family classifier and
the TEV feasibility report need.  It does not claim to be a full compiler.

No game bytes live in this repository.  The callers read the retail ``.fxp``
packages from the local install (read-only) and keep derived tables only.
"""

from __future__ import annotations

import struct
from collections import Counter
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

# D3DSIO_* opcodes (d3d9types.h).  Kept local so tools/shaders does not depend
# on the inventory module's private tables.
OPCODE_NAMES = {
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
NAME_TO_OPCODE = {name: op for op, name in OPCODE_NAMES.items()}

# Register types after the SM2 register split (d3d9types.h D3DSPR_*).
REG_TEMP = 0
REG_INPUT = 1
REG_CONST = 2
REG_ADDR = 3
REG_RASTOUT = 4
REG_ATTROUT = 5
REG_OUTPUT = 6
REG_CONSTINT = 7
REG_COLOROUT = 8
REG_DEPTHOUT = 9
REG_SAMPLER = 10
REG_CONSTBOOL = 11
REG_LABEL = 12
REG_PREDICATE = 13
REG_LOOP = 15
REG_MISCTYPE = 17

REG_TYPE_NAMES = {
    REG_TEMP: "r", REG_INPUT: "v", REG_CONST: "c", REG_ADDR: "a",
    REG_RASTOUT: "rast", REG_ATTROUT: "attr", REG_OUTPUT: "o",
    REG_CONSTINT: "i", REG_COLOROUT: "oC", REG_DEPTHOUT: "oDepth",
    REG_SAMPLER: "s", REG_CONSTBOOL: "b", REG_LABEL: "l",
    REG_PREDICATE: "p", REG_LOOP: "aL", REG_MISCTYPE: "misc",
}

#: ``texld``/``texldd``/``texldl`` opcodes; the first two control names apply
#: only to plain ``texld`` (0x42), where the instruction token carries
#: ``D3DSI_TEXLD_PROJECT``/``D3DSI_TEXLD_BIAS`` in bits 16-23.
TEXLD_OPS = frozenset({0x42, 0x5D, 0x5F})
TEXLD_PROJECT = 0x01
TEXLD_BIAS = 0x02

#: Opcodes whose first operand is a source, not a destination.  Mirrors the
#: MojoShader instruction table (``parse_args_S``/``parse_args_SS``/NULL).
NO_DST_OPCODES = frozenset({
    0x00,  # nop
    0x19, 0x1A,                # call, callnz
    0x1B, 0x1C, 0x1D,          # loop, ret, endloop
    0x1E,                      # label
    0x26, 0x27,                # rep, endrep
    0x28, 0x29, 0x2A, 0x2B,    # if, ifc, else, endif
    0x2C, 0x2D,                # break, breakc
    0x60,                      # breakp
})

USAGE_NAMES = {
    0: "position", 1: "blendweight", 2: "blendindices", 3: "normal",
    4: "psize", 5: "texcoord", 6: "tangent", 7: "binormal",
    8: "tessfactor", 9: "positiont", 10: "color", 11: "fog",
    12: "depth", 13: "sample",
}
TEXTURE_TYPE_NAMES = {0: "unknown", 1: "unknown1", 2: "2d", 3: "cube", 4: "volume"}

SWIZZLE_NAMES = "xyzw"


def reg_type(token: int) -> int:
    return ((token >> 28) & 0x7) | ((token & 0x1800) >> 8)


def reg_number(token: int) -> int:
    return token & 0x7FF


def write_mask(token: int) -> int:
    return (token >> 16) & 0xF


def dst_modifier(token: int) -> int:
    return (token >> 20) & 0xF


def swizzle(token: int) -> str:
    bits = (token >> 16) & 0xFF
    return "".join(SWIZZLE_NAMES[(bits >> (2 * i)) & 3] for i in range(4))


def source_modifier(token: int) -> int:
    return (token >> 24) & 0xF


SRC_MODIFIER_NAMES = {
    0: "", 1: "neg", 2: "bias", 3: "biasneg", 4: "sign", 5: "signneg",
    6: "complement", 7: "x2", 8: "x2neg", 9: "dz", 10: "dw", 11: "abs",
    12: "absneg", 13: "not",
}


def is_relative(token: int) -> bool:
    """``D3DSP_ADDRESSMODE_RELATIVE`` (bit 13) on a source token."""
    return bool((token >> 13) & 0x1)


@dataclass(frozen=True)
class Relative:
    """Full relative-addressing tuple for an indexed source.

    ``base`` is the register being indexed (the source token itself),
    ``index_type``/``index_index`` the address or loop register (``a0``/``aL``),
    ``component`` its channel and ``offset`` the extra constant carried in the
    relative token.  fxc encodes ``c[a0.x + 12]`` as base ``c12`` with a zero
    offset, so the base register number is part of the tuple.
    """

    base_type: int
    base_index: int
    index_type: int
    index_index: int
    component: str
    offset: int

    def as_dict(self) -> dict:
        return {"base_type": self.base_type, "base_index": self.base_index,
                "index_type": self.index_type, "index_index": self.index_index,
                "component": self.component, "offset": self.offset}

    def name(self) -> str:
        text = REG_TYPE_NAMES.get(self.index_type,
                                  "t%d" % self.index_type) + str(self.index_index)
        if self.component:
            text += "." + self.component
        if self.offset:
            text += " + %d" % self.offset
        return text


@dataclass(frozen=True)
class Src:
    token: int
    reg_type: int
    index: int
    swizzle: str
    modifier: int
    relative: Optional[Relative] = None

    @property
    def type_name(self) -> str:
        return REG_TYPE_NAMES.get(self.reg_type, "t%d" % self.reg_type)

    def name(self) -> str:
        text = "%s%d" % (self.type_name, self.index)
        if self.relative is not None:
            text += "[%s]" % self.relative.name()
        if self.swizzle != "xyzw":
            text += "." + self.swizzle
        if self.modifier:
            text += " (%s)" % SRC_MODIFIER_NAMES.get(self.modifier, "mod%d" % self.modifier)
        return text

    def as_dict(self) -> dict:
        return {
            "type": self.reg_type,
            "index": self.index,
            "swizzle": self.swizzle,
            "modifier": self.modifier,
            "relative": self.relative.as_dict() if self.relative is not None else None,
        }


@dataclass(frozen=True)
class Dst:
    token: int
    reg_type: int
    index: int
    mask: int
    modifier: int

    @property
    def type_name(self) -> str:
        return REG_TYPE_NAMES.get(self.reg_type, "t%d" % self.reg_type)

    def name(self) -> str:
        text = "%s%d" % (self.type_name, self.index)
        mask = (self.mask & 0xF) if self.mask else 0
        if mask != 0xF:
            text += "." + "".join(SWIZZLE_NAMES[i] for i in range(4) if mask & (1 << i))
        if self.modifier:
            text += " (dstmod%d)" % self.modifier
        return text

    def as_dict(self) -> dict:
        return {"type": self.reg_type, "index": self.index, "mask": self.mask,
                "modifier": self.modifier}


@dataclass
class Instruction:
    opcode: int
    offset: int
    words: int
    dst: Optional[Dst] = None
    srcs: Tuple[Src, ...] = ()
    literal: Tuple[float, ...] = ()
    decl: Optional[dict] = None
    control: int = 0

    @property
    def control_name(self) -> str:
        if self.opcode not in TEXLD_OPS:
            return ""
        name = "texld"
        if self.opcode == 0x42:
            if self.control & TEXLD_PROJECT:
                name = "texldp"
            if self.control & TEXLD_BIAS:
                name = "texldb"
        return name

    @property
    def projected(self) -> bool:
        return self.opcode == 0x42 and bool(self.control & TEXLD_PROJECT)

    @property
    def biased(self) -> bool:
        return self.opcode == 0x42 and bool(self.control & TEXLD_BIAS)

    @property
    def texture(self) -> Optional[dict]:
        """``texld`` sampler/coordinate pair plus its control bits."""
        if self.opcode not in TEXLD_OPS or len(self.srcs) < 2:
            return None
        return {"control": self.control, "projected": self.projected,
                "biased": self.biased,
                "coord": self.srcs[0], "sampler": self.srcs[1]}

    @property
    def name(self) -> str:
        if self.opcode == 0x42 and self.control in (TEXLD_PROJECT, TEXLD_BIAS):
            return self.control_name
        return OPCODE_NAMES.get(self.opcode, "op_0x%02x" % self.opcode)

    def text(self) -> str:
        parts = [self.name]
        if self.dst is not None:
            parts.append(self.dst.name())
        parts.extend(src.name() for src in self.srcs)
        if self.literal:
            parts.append("{%s}" % ", ".join("%g" % v for v in self.literal))
        return " ".join(parts)

    def as_dict(self) -> dict:
        return {
            "opcode": self.opcode,
            "name": self.name,
            "offset": self.offset,
            "words": self.words,
            "control": self.control,
            "dst": self.dst.as_dict() if self.dst is not None else None,
            "srcs": [src.as_dict() for src in self.srcs],
            "literal": list(self.literal),
            "decl": self.decl,
        }


@dataclass
class ConstantInfo:
    name: str
    register_set: int
    register_index: int
    register_count: int

    def as_dict(self) -> dict:
        return {"name": self.name, "set": self.register_set,
                "index": self.register_index, "count": self.register_count}


@dataclass
class Declaration:
    usage: int
    usage_index: int
    reg_type: int
    reg_index: int
    texture_type: int = 0

    @property
    def usage_name(self) -> str:
        return USAGE_NAMES.get(self.usage, "usage%d" % self.usage)

    def as_dict(self) -> dict:
        return {"usage": self.usage_name, "usage_index": self.usage_index,
                "reg_type": self.reg_type, "reg_index": self.reg_index,
                "texture_type": TEXTURE_TYPE_NAMES.get(self.texture_type, "?")}


@dataclass
class Program:
    version: int
    vertex: bool
    instructions: List[Instruction] = field(default_factory=list)
    constants: List[ConstantInfo] = field(default_factory=list)
    declarations: List[Declaration] = field(default_factory=list)
    error: Optional[str] = None
    comments: int = 0

    @property
    def opcodes(self) -> List[int]:
        return [ins.opcode for ins in self.instructions]

    @property
    def opcode_counter(self) -> Counter:
        return Counter(self.opcodes)

    def texture_samples(self) -> int:
        return sum(1 for ins in self.instructions if ins.opcode in (0x42, 0x5D, 0x5F))

    def branches(self) -> int:
        return sum(1 for ins in self.instructions
                   if ins.opcode in (0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x26, 0x27,
                                     0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x60))

    def loops(self) -> int:
        return sum(1 for ins in self.instructions if ins.opcode in (0x1B, 0x1D, 0x26, 0x27))


def _u32(data: bytes, pos: int) -> int:
    return struct.unpack_from("<I", data, pos)[0]


def parse_constants(code: bytes) -> List[ConstantInfo]:
    """Decode the first ``CTAB`` comment.

    Empirically the D3DXSHADER_CONSTANTTABLE lives at the ``CTAB`` fourcc + 4
    and every name/creator/target offset is relative to that struct start.
    The trailing ``0xab`` bytes in the chunk metadata are unrelated to CTAB.
    """
    marker = code.find(b"CTAB")
    if marker < 0:
        return []
    base = marker + 4
    if base + 28 > len(code):
        return []
    count = struct.unpack_from("<H", code, base + 12)[0]
    info_offset = _u32(code, base + 16)
    out: List[ConstantInfo] = []
    for i in range(min(count, 256)):
        entry = base + info_offset + i * 20
        if entry + 20 > len(code):
            break
        name_offset = _u32(code, entry)
        if not name_offset:
            continue
        start = base + name_offset
        end = code.find(b"\x00", start)
        if end < 0 or start >= len(code):
            continue
        name = code[start:end].decode("latin-1", "replace")
        register = _u32(code, entry + 4)
        register_set = register & 0xFFFF
        register_index = (register >> 16) & 0xFFFF
        register_count = _u32(code, entry + 12)
        out.append(ConstantInfo(name, register_set, register_index, register_count))
    return out


def _parse_decl(decl_token: int, dst_token: int) -> Declaration:
    usage = decl_token & 0x1F
    usage_index = (decl_token >> 16) & 0xF
    texture_type = (decl_token >> 27) & 0xF
    return Declaration(usage, usage_index, reg_type(dst_token),
                       reg_number(dst_token), texture_type)


def parse_program(code: bytes) -> Program:
    """Decode a chunk, including its CTAB and dcl declarations."""
    if len(code) < 8:
        return Program(0, False, error="bytecode too short")
    version = _u32(code, 0)
    vertex = (version >> 16) == 0xFFFE
    program = Program(version, vertex)
    program.constants = parse_constants(code)
    pos = 4
    while pos + 4 <= len(code):
        token = _u32(code, pos)
        opcode = token & 0xFFFF
        if opcode == 0xFFFF:
            return program
        if opcode == 0xFFFE:
            words = (token >> 16) & 0x7FFF
            if pos + 4 + 4 * words > len(code):
                program.error = "truncated comment"
                return program
            program.comments += 1
            pos += 4 + 4 * words
            continue
        total_words = 1 + ((token >> 24) & 0xF)
        if pos + 4 * total_words > len(code):
            program.error = "instruction runs past bytecode"
            return program
        operands = [_u32(code, pos + 4 * i) for i in range(1, total_words)]
        ins = Instruction(opcode=opcode, offset=pos, words=total_words)
        if opcode == 0x42:
            # Only plain TEXLD uses bits 16-23 for PROJECT/BIAS; TEXLDD/TEXLDL
            # have no control word.
            ins.control = (token >> 16) & 0xFF
        try:
            if opcode == 0x1F and len(operands) >= 2:
                ins.decl = _parse_decl(operands[0], operands[1]).as_dict()
                program.declarations.append(_parse_decl(operands[0], operands[1]))
            elif opcode == 0x51 and len(operands) >= 5:  # def
                ins.dst = _dst(operands[0])
                ins.literal = tuple(struct.unpack("<4f", struct.pack(
                    "<4I", *operands[1:5])))
            elif opcode == 0x30 and len(operands) >= 5:  # defi
                ins.dst = _dst(operands[0])
                ins.literal = tuple(float(v) for v in operands[1:5])
            elif opcode == 0x2F:  # defb: destination + one literal dword
                ins.dst = _dst(operands[0])
                if len(operands) >= 2:
                    ins.literal = (float(operands[1] & 0xFFFF),)
            elif opcode in NO_DST_OPCODES:
                # Branch/loop/label operands are source-format tokens: there is
                # no destination to wrap.  A relative source still consumes a
                # second token.
                ins.srcs = _parse_srcs(operands, 0)
            elif operands:
                ins.dst = _dst(operands[0])
                ins.srcs = _parse_srcs(operands, 1)
        except (struct.error, IndexError) as exc:
            program.error = "operand decode failed at %d: %s" % (pos, exc)
            return program
        program.instructions.append(ins)
        pos += 4 * total_words
    program.error = "missing END token"
    return program


def _src(token: int, relative: Optional[Relative] = None) -> Src:
    return Src(token, reg_type(token), reg_number(token), swizzle(token),
               source_modifier(token), relative)


def _parse_srcs(operands: Sequence[int], start: int) -> Tuple[Src, ...]:
    """Decode source operands, swallowing relative-address tokens.

    A relative source is followed by one extra token describing the index
    register; that token is not an operand of its own.
    """
    srcs: List[Src] = []
    index = start
    while index < len(operands):
        token = operands[index]
        relative: Optional[Relative] = None
        step = 1
        if is_relative(token) and index + 1 < len(operands):
            rel = operands[index + 1]
            relative = Relative(
                base_type=reg_type(token), base_index=reg_number(token),
                index_type=reg_type(rel), index_index=reg_number(rel),
                component=SWIZZLE_NAMES[(rel >> 16) & 3],
                offset=reg_number(rel),
            )
            step = 2
        srcs.append(_src(token, relative))
        index += step
    return tuple(srcs)


def _dst(token: int) -> Dst:
    return Dst(token, reg_type(token), reg_number(token), write_mask(token),
               dst_modifier(token))


# ---------------------------------------------------------------------------
# Feature extraction
# ---------------------------------------------------------------------------

#: Opcodes the first TEV translator family accepts (mirrors d3d9_tev.c).
TEV_SIMPLE_OPCODES = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x12, 0x1F, 0x42, 0x51}


def feature_vector(program: Program) -> dict:
    counter = program.opcode_counter
    texld = program.texture_samples()
    samplers = [d for d in program.declarations if d.reg_type == REG_SAMPLER]
    inputs = [d for d in program.declarations if d.reg_type == REG_INPUT]
    constants_used = set()
    out_regs = set()
    projected = 0
    biased = 0
    for ins in program.instructions:
        for src in ins.srcs:
            if src.reg_type == REG_CONST:
                constants_used.add(src.index)
        if ins.dst is not None and ins.dst.reg_type == REG_COLOROUT:
            out_regs.add(ins.dst.index)
        if ins.projected:
            projected += 1
        if ins.biased:
            biased += 1
    return {
        "opcodes": set(counter),
        "instructions": len(program.instructions),
        "texture_samples": texld,
        "projected_samples": projected,
        "biased_samples": biased,
        "samplers": [d.reg_index for d in samplers],
        "sampler_types": [d.texture_type for d in samplers],
        "inputs": [(d.usage_name, d.usage_index, d.reg_index) for d in inputs],
        "input_usages": {d.usage_name for d in inputs},
        "constants_used": sorted(constants_used),
        "constant_names": [c.name for c in program.constants],
        "outputs": sorted(out_regs),
        "branches": program.branches(),
        "loops": program.loops(),
        "mrt": any(i >= 1 for i in out_regs),
        "simple_opcodes": set(counter) <= TEV_SIMPLE_OPCODES and program.error is None,
        "error": program.error,
    }


def constant_name_map(program: Program) -> Dict[Tuple[int, int], str]:
    """(register_set, register_index) -> CTAB name."""
    return {(c.register_set, c.register_index): c.name for c in program.constants}


def used_constant_names(program: Program) -> List[str]:
    mapping = constant_name_map(program)
    names = set()
    for ins in program.instructions:
        for src in ins.srcs:
            if src.reg_type == REG_CONST:
                name = mapping.get((2, src.index))
                if name:
                    names.add(name)
    return sorted(names)


__all__ = [
    "OPCODE_NAMES", "NAME_TO_OPCODE", "REG_TEMP", "REG_INPUT", "REG_CONST",
    "REG_COLOROUT", "REG_DEPTHOUT", "REG_SAMPLER", "REG_LOOP", "USAGE_NAMES",
    "Src", "Dst", "Relative", "Instruction", "ConstantInfo", "Declaration",
    "Program", "parse_program", "parse_constants", "feature_vector",
    "constant_name_map", "used_constant_names", "swizzle", "write_mask",
    "reg_type", "reg_number", "is_relative", "TEXLD_OPS", "TEXLD_PROJECT",
    "TEXLD_BIAS", "NO_DST_OPCODES", "TEV_SIMPLE_OPCODES",
]
