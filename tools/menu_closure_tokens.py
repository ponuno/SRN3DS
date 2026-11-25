#!/usr/bin/env python3
"""Minimal D3D9 token descriptor shared by the fixture extractor and tests.

This mirrors runtime/d3d9_menu_closure.c's walker so the generated contract and
the C certifier cannot disagree about what a declaration or instruction is.
"""
from __future__ import annotations

import struct

COMMENT = 0xFFFE
PHASE = 0xFFFD
END = 0xFFFF
DCL = 0x1F
TEXLD = 0x42
BRANCH_OPS = {0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x26, 0x27, 0x28, 0x29, 0x2A,
              0x2B, 0x2C, 0x2D, 0x60}

REG_INPUT = 1
REG_OUTPUT = 6
REG_SAMPLER = 10

USAGE_NAMES = {
    0: "position", 1: "blendweight", 2: "blendindices", 3: "normal",
    4: "psize", 5: "texcoord", 6: "tangent", 7: "binormal", 8: "tessfactor",
    9: "positiont", 10: "color", 11: "fog", 12: "depth", 13: "sample",
}
USAGE_REVERSE = {name: value for value, name in USAGE_NAMES.items()}


def u32(code: bytes, pos: int) -> int:
    return struct.unpack_from("<I", code, pos)[0]


def describe(code: bytes) -> dict:
    if len(code) < 8:
        return {"error": "too short"}
    version = u32(code, 0)
    pos = 4
    instructions = 0
    opcodes = []
    decls = []
    outputs = set()
    texld = 0
    branches = 0
    end = False
    words = len(code) // 4
    while pos + 4 <= len(code):
        token = u32(code, pos)
        op = token & 0xFFFF
        if op == END:
            pos += 4
            end = True
            break
        if op == COMMENT:
            length = (token >> 16) & 0x7FFF
            pos += 4 + 4 * length
            continue
        if op == PHASE:
            pos += 4
            continue
        if op > 96:
            return {"error": "opcode %d" % op, "at_word": pos // 4}
        extra = (token >> 24) & 0xF
        if pos + 4 + extra * 4 > len(code):
            return {"error": "truncated", "at_word": pos // 4}
        instructions += 1
        opcodes.append(op)
        if op == DCL and extra == 2:
            usage_token = u32(code, pos + 4)
            dst = u32(code, pos + 8)
            decls.append({
                "usage": usage_token & 0x1F,
                "usage_name": USAGE_NAMES.get(usage_token & 0x1F, "usage%d" % (usage_token & 0x1F)),
                "index": (usage_token >> 16) & 0xF,
                "texture_type": (usage_token >> 27) & 0xF if (usage_token >> 31) else 0,
                "reg_type": (dst >> 28) & 0x7,
                "reg_index": dst & 0x7FF,
            })
        elif extra >= 1:
            dst = u32(code, pos + 4)
            reg_type = (dst >> 28) & 0x7
            reg_index = dst & 0x7FF
            if reg_type == REG_OUTPUT:
                outputs.add(reg_index)
        if op == DCL:
            pass
        if op == TEXLD:
            texld += 1
        if op in BRANCH_OPS:
            branches += 1
        pos += 4 + extra * 4
    return {
        "error": None if end else "missing END",
        "version": version,
        "words": words,
        "used_words": pos // 4,
        "used_bytes": pos,
        "instructions": instructions,
        "opcodes": opcodes,
        "declarations": decls,
        "outputs": sorted(outputs),
        "texld": texld,
        "branches": branches,
    }
