#!/usr/bin/env python3
"""Read-only Skyrim Papyrus PEX 3.x dumper.

The parser deliberately has no third-party dependencies.  It reads the
big-endian Skyrim layout (magic bytes FA 57 C0 DE, game id 1), including the
string table, debug line table, objects, properties, states/functions and
typed bytecode operands.  Fallout/Starfield layouts are rejected rather than
silently decoded with the wrong field widths.

Examples:
    python tools/pex_dump.py Script.pex
    python tools/pex_dump.py Script.pex --strings --functions --instructions
    python tools/pex_dump.py a.pex b.pex --json > pex.json
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from typing import Any, Iterable

SKYRIM_MAGIC = b"\xfa\x57\xc0\xde"  # little-endian DWORD 0xDEC057FA
SKYRIM_GAME_ID = 1
MAX_STRING_BYTES = 16 * 1024 * 1024
MAX_TABLE_ENTRIES = 1_000_000
MAX_INSTRUCTIONS = 1_000_000

# Champollion v1.3.2 Pex/Instruction.cpp.  Skyrim 3.2 uses opcodes 0x00..0x24;
# the later names are retained so a newer file gets a precise diagnostic rather
# than an operand-width guess.  A non-Skyrim opcode is reported in warnings.
OPCODES: tuple[tuple[str, int, bool], ...] = (
    ("nop", 0, False),
    ("iadd", 3, False),
    ("fadd", 3, False),
    ("isub", 3, False),
    ("fsub", 3, False),
    ("imul", 3, False),
    ("fmul", 3, False),
    ("idiv", 3, False),
    ("fdiv", 3, False),
    ("imod", 3, False),
    ("not", 2, False),
    ("ineg", 2, False),
    ("fneg", 2, False),
    ("assign", 2, False),
    ("cast", 2, False),
    ("cmp_eq", 3, False),
    ("cmp_lt", 3, False),
    ("cmp_lte", 3, False),
    ("cmp_gt", 3, False),
    ("cmp_gte", 3, False),
    ("jmp", 1, False),
    ("jmpt", 2, False),
    ("jmpf", 2, False),
    ("callmethod", 3, True),
    ("callparent", 2, True),
    ("callstatic", 3, True),
    ("return", 1, False),
    ("strcat", 3, False),
    ("propget", 3, False),
    ("propset", 3, False),
    ("array_create", 2, False),
    ("array_length", 2, False),
    ("array_getelement", 3, False),
    ("array_setelement", 3, False),
    ("array_findelement", 4, False),
    ("array_rfindelement", 4, False),
    ("is", 3, False),
    ("struct_create", 1, False),
    ("struct_get", 3, False),
    ("struct_set", 3, False),
    ("array_findstruct", 5, False),
    ("array_rfindstruct", 5, False),
    ("array_add", 3, False),
    ("array_insert", 3, False),
    ("array_removelast", 1, False),
    ("array_remove", 3, False),
    ("array_clear", 1, False),
    ("array_getallmatchingstructs", 6, False),
    ("lock_guards", 0, True),
    ("unlock_guards", 0, True),
    ("try_lock_guards", 1, True),
)
SKYRIM_MAX_OPCODE = 0x24
VALUE_TYPES = {0: "none", 1: "identifier", 2: "string", 3: "integer", 4: "float", 5: "bool"}


class PexError(ValueError):
    """A bounded, offset-bearing parse failure."""


@dataclass
class Value:
    type: str
    value: Any = None
    string_index: int | None = None

    def as_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {"type": self.type}
        if self.type in ("identifier", "string"):
            out["string_index"] = self.string_index
            out["value"] = self.value
        elif self.type == "float":
            out["value"] = self.value if math.isfinite(self.value) else str(self.value)
        else:
            out["value"] = self.value
        return out

    def display(self, strings: list[str]) -> str:
        if self.type in ("identifier", "string"):
            text = self.value or ""
            if self.type == "string":
                text = json.dumps(text, ensure_ascii=False)
            return f"{text}[s{self.string_index}]"
        if self.type == "none":
            return "None"
        if self.type == "bool":
            return "True" if self.value else "False"
        return str(self.value)


@dataclass
class Instruction:
    ordinal: int
    offset: int
    opcode: int
    name: str
    args: list[Value]
    varargs: list[Value]
    byte_size: int

    def as_dict(self) -> dict[str, Any]:
        return {
            "ordinal": self.ordinal,
            "offset": self.offset,
            "opcode": self.opcode,
            "name": self.name,
            "byte_size": self.byte_size,
            "args": [v.as_dict() for v in self.args],
            "varargs": [v.as_dict() for v in self.varargs],
        }

    def display(self, strings: list[str]) -> str:
        values = self.args + self.varargs
        suffix = (" " + " ".join(v.display(strings) for v in values)) if values else ""
        return f"{self.offset:04x} {self.opcode:02x} {self.name}{suffix}"


@dataclass
class DebugFunction:
    object_name: str
    state_name: str
    function_name: str
    function_type: int
    line_numbers: list[int]


@dataclass
class Parameter:
    name: str
    type_name: str


@dataclass
class Function:
    name: str
    return_type: str
    docstring: str
    user_flags: int
    flags: int
    params: list[Parameter]
    locals: list[Parameter]
    instructions: list[Instruction]
    object_name: str = ""
    state_name: str = ""
    role: str = "method"
    debug_lines: list[int] = field(default_factory=list)

    @property
    def is_global(self) -> bool:
        return bool(self.flags & 0x01)

    @property
    def is_native(self) -> bool:
        return bool(self.flags & 0x02)

    @property
    def qualified_name(self) -> str:
        parts = [self.object_name]
        if self.state_name:
            parts.append(self.state_name)
        parts.append(self.name)
        return ".".join(parts)

    def as_dict(self, include_instructions: bool = False) -> dict[str, Any]:
        out: dict[str, Any] = {
            "name": self.name,
            "qualified_name": self.qualified_name,
            "object": self.object_name,
            "state": self.state_name,
            "role": self.role,
            "return_type": self.return_type,
            "docstring": self.docstring,
            "user_flags": self.user_flags,
            "flags": self.flags,
            "is_global": self.is_global,
            "is_native": self.is_native,
            "params": [{"name": p.name, "type": p.type_name} for p in self.params],
            "locals": [{"name": p.name, "type": p.type_name} for p in self.locals],
            "instruction_count": len(self.instructions),
            "code_bytes": sum(i.byte_size for i in self.instructions),
            "debug_line_count": len(self.debug_lines),
        }
        if include_instructions:
            out["instructions"] = [i.as_dict() for i in self.instructions]
        return out


@dataclass
class Variable:
    name: str
    type_name: str
    user_flags: int
    default_value: Value


@dataclass
class Property:
    name: str
    type_name: str
    docstring: str
    user_flags: int
    flags: int
    auto_var_name: str | None
    functions: list[Function]


@dataclass
class State:
    name: str
    functions: list[Function]


@dataclass
class Object:
    name: str
    size_bytes: int
    parent_class: str
    docstring: str
    user_flags: int
    auto_state_name: str
    variables: list[Variable]
    properties: list[Property]
    states: list[State]

    @property
    def functions(self) -> list[Function]:
        out = [f for p in self.properties for f in p.functions]
        out.extend(f for s in self.states for f in s.functions)
        return out


@dataclass
class Pex:
    path: str
    sha256: str
    file_bytes: int
    major_version: int
    minor_version: int
    game_id: int
    compilation_time: int
    source_name: str
    user_name: str
    computer_name: str
    strings: list[str]
    debug_present: bool
    debug_modification_time: int
    debug_functions: list[DebugFunction]
    user_flags: list[tuple[str, int]]
    objects: list[Object]
    warnings: list[str]
    trailing_bytes: int

    @property
    def functions(self) -> list[Function]:
        return [f for obj in self.objects for f in obj.functions]

    @property
    def instructions(self) -> Iterable[Instruction]:
        return (i for f in self.functions for i in f.instructions)

    @property
    def opcode_histogram(self) -> Counter[int]:
        return Counter(i.opcode for i in self.instructions)

    def summary(self, include_strings: bool = False, include_functions: bool = False,
                include_instructions: bool = False) -> dict[str, Any]:
        histogram = self.opcode_histogram
        out: dict[str, Any] = {
            "path": self.path,
            "sha256": self.sha256,
            "file_bytes": self.file_bytes,
            "magic": "0xDEC057FA",
            "endianness": "big",
            "major_version": self.major_version,
            "minor_version": self.minor_version,
            "version": f"{self.major_version}.{self.minor_version}",
            "game_id": self.game_id,
            "compilation_time": self.compilation_time,
            "source_name": self.source_name,
            "user_name": self.user_name,
            "computer_name": self.computer_name,
            "string_count": len(self.strings),
            "debug_present": self.debug_present,
            "debug_modification_time": self.debug_modification_time,
            "debug_function_count": len(self.debug_functions),
            "user_flag_count": len(self.user_flags),
            "object_count": len(self.objects),
            "state_count": sum(len(o.states) for o in self.objects),
            "property_count": sum(len(o.properties) for o in self.objects),
            "variable_count": sum(len(o.variables) for o in self.objects),
            "function_count": len(self.functions),
            "instruction_count": sum(histogram.values()),
            "code_bytes": sum(i.byte_size for i in self.instructions),
            "opcode_histogram": [
                {"opcode": op, "name": OPCODES[op][0] if op < len(OPCODES) else "unknown",
                 "count": count}
                for op, count in sorted(histogram.items())
            ],
            "objects": [
                {
                    "name": o.name,
                    "parent_class": o.parent_class,
                    "auto_state": o.auto_state_name,
                    "size_bytes": o.size_bytes,
                    "variable_count": len(o.variables),
                    "property_count": len(o.properties),
                    "state_count": len(o.states),
                    "function_count": len(o.functions),
                }
                for o in self.objects
            ],
            "warnings": self.warnings,
            "trailing_bytes": self.trailing_bytes,
        }
        if include_strings:
            out["strings"] = self.strings
        if include_functions or include_instructions:
            out["functions"] = [f.as_dict(include_instructions) for f in self.functions]
        return out


class Reader:
    def __init__(self, data: bytes):
        self.data = memoryview(data)
        self.pos = 0

    def fail(self, message: str) -> PexError:
        raise PexError(f"offset 0x{self.pos:x}: {message}")

    def take(self, count: int, what: str) -> memoryview:
        if count < 0 or count > len(self.data) - self.pos:
            self.fail(f"truncated {what}: need {count} bytes, have {len(self.data) - self.pos}")
        out = self.data[self.pos:self.pos + count]
        self.pos += count
        return out

    def u8(self, what: str = "byte") -> int:
        return self.take(1, what)[0]

    def u16(self, what: str = "uint16") -> int:
        return struct.unpack(">H", self.take(2, what))[0]

    def u32(self, what: str = "uint32") -> int:
        return struct.unpack(">I", self.take(4, what))[0]

    def i32(self, what: str = "int32") -> int:
        return struct.unpack(">i", self.take(4, what))[0]

    def f32(self, what: str = "float32") -> float:
        return struct.unpack(">f", self.take(4, what))[0]

    def u64(self, what: str = "uint64") -> int:
        return struct.unpack(">Q", self.take(8, what))[0]

    def count(self, what: str) -> int:
        value = self.u16(what)
        if value > MAX_TABLE_ENTRIES:
            self.fail(f"implausible {what}: {value}")
        return value

    def string(self, what: str = "string") -> str:
        length = self.u16(f"{what} length")
        if length > MAX_STRING_BYTES:
            self.fail(f"implausible {what} length: {length}")
        raw = bytes(self.take(length, what))
        return raw.decode("utf-8", errors="replace")


def parse_pex(data: bytes, path: str = "<memory>") -> Pex:
    r = Reader(data)
    if bytes(r.take(4, "magic")) != SKYRIM_MAGIC:
        r.fail("not a big-endian Skyrim PEX (expected FA 57 C0 DE / 0xDEC057FA)")
    major = r.u8("major version")
    minor = r.u8("minor version")
    game_id = r.u16("game id")
    if major != 3:
        r.fail(f"unsupported PEX major version {major}; this tool implements Skyrim PEX 3.x")
    if game_id != SKYRIM_GAME_ID:
        r.fail(f"unsupported game id {game_id}; this tool implements Skyrim game id 1")
    compilation_time = r.u64("compilation time")
    source_name = r.string("source name")
    user_name = r.string("user name")
    computer_name = r.string("computer name")

    string_count = r.count("string count")
    strings = [r.string(f"string[{i}]") for i in range(string_count)]

    def string_index(what: str) -> int:
        index = r.u16(what)
        if index >= len(strings):
            r.fail(f"{what} index {index} outside string table ({len(strings)})")
        return index

    def s(index: int) -> str:
        return strings[index]

    debug_present = bool(r.u8("debug-present flag"))
    debug_time = 0
    debug_functions: list[DebugFunction] = []
    if debug_present:
        debug_time = r.u64("debug modification time")
        debug_count = r.count("debug function count")
        for i in range(debug_count):
            object_name = s(string_index(f"debug[{i}] object"))
            state_name = s(string_index(f"debug[{i}] state"))
            function_name = s(string_index(f"debug[{i}] function"))
            function_type = r.u8(f"debug[{i}] function type")
            line_count = r.count(f"debug[{i}] line count")
            lines = [r.u16(f"debug[{i}] line[{j}]") for j in range(line_count)]
            debug_functions.append(DebugFunction(object_name, state_name, function_name,
                                                 function_type, lines))

    user_flag_count = r.count("user-flag count")
    user_flags = [(s(string_index(f"user flag[{i}] name")), r.u8(f"user flag[{i}] index"))
                  for i in range(user_flag_count)]

    def value(what: str) -> Value:
        kind = r.u8(f"{what} value type")
        if kind == 0:
            return Value("none", None)
        if kind == 1:
            index = string_index(f"{what} identifier")
            return Value("identifier", s(index), index)
        if kind == 2:
            index = string_index(f"{what} string")
            return Value("string", s(index), index)
        if kind == 3:
            return Value("integer", r.i32(f"{what} integer"))
        if kind == 4:
            return Value("float", r.f32(f"{what} float"))
        if kind == 5:
            return Value("bool", bool(r.u8(f"{what} bool")))
        r.fail(f"unknown {what} value type {kind}")

    def typed_names(what: str) -> list[Parameter]:
        count = r.count(f"{what} count")
        return [Parameter(s(string_index(f"{what}[{i}] name")),
                          s(string_index(f"{what}[{i}] type"))) for i in range(count)]

    def instructions(what: str) -> list[Instruction]:
        count = r.count(f"{what} instruction count")
        if count > MAX_INSTRUCTIONS:
            r.fail(f"implausible {what} instruction count: {count}")
        code_start = r.pos
        out: list[Instruction] = []
        for ordinal in range(count):
            offset = r.pos - code_start
            opcode = r.u8(f"{what} instruction[{ordinal}] opcode")
            if opcode >= len(OPCODES):
                r.fail(f"unknown opcode 0x{opcode:02x} in {what} instruction[{ordinal}]")
            name, arg_count, has_varargs = OPCODES[opcode]
            args = [value(f"{what} instruction[{ordinal}] arg[{j}]") for j in range(arg_count)]
            varargs: list[Value] = []
            if has_varargs:
                vararg_count_value = value(f"{what} instruction[{ordinal}] vararg count")
                if vararg_count_value.type != "integer":
                    r.fail(f"{what} instruction[{ordinal}] vararg count is not an integer")
                vararg_count = vararg_count_value.value
                if vararg_count < 0 or vararg_count > MAX_TABLE_ENTRIES:
                    r.fail(f"{what} instruction[{ordinal}] invalid vararg count {vararg_count}")
                varargs = [value(f"{what} instruction[{ordinal}] vararg[{j}]")
                           for j in range(vararg_count)]
            out.append(Instruction(ordinal, offset, opcode, name, args, varargs,
                                   r.pos - code_start - offset))
        return out

    def function(what: str, name: str, object_name: str, state_name: str,
                 role: str) -> Function:
        return_type = s(string_index(f"{what} return type"))
        docstring = s(string_index(f"{what} docstring"))
        flags_user = r.u32(f"{what} user flags")
        flags = r.u8(f"{what} flags")
        params = typed_names(f"{what} params")
        locals_ = typed_names(f"{what} locals")
        code = instructions(what)
        return Function(name, return_type, docstring, flags_user, flags, params, locals_,
                        code, object_name, state_name, role)

    object_count = r.count("object count")
    objects: list[Object] = []
    for oi in range(object_count):
        object_name = s(string_index(f"object[{oi}] name"))
        size_bytes = r.u32(f"object[{oi}] size")
        parent_class = s(string_index(f"object[{oi}] parent class"))
        docstring = s(string_index(f"object[{oi}] docstring"))
        obj_user_flags = r.u32(f"object[{oi}] user flags")
        auto_state_name = s(string_index(f"object[{oi}] auto state"))

        variable_count = r.count(f"object[{oi}] variable count")
        variables: list[Variable] = []
        for vi in range(variable_count):
            name = s(string_index(f"object[{oi}] variable[{vi}] name"))
            type_name = s(string_index(f"object[{oi}] variable[{vi}] type"))
            vf = r.u32(f"object[{oi}] variable[{vi}] user flags")
            default = value(f"object[{oi}] variable[{vi}] default")
            variables.append(Variable(name, type_name, vf, default))

        property_count = r.count(f"object[{oi}] property count")
        properties: list[Property] = []
        for pi in range(property_count):
            name = s(string_index(f"object[{oi}] property[{pi}] name"))
            type_name = s(string_index(f"object[{oi}] property[{pi}] type"))
            pdoc = s(string_index(f"object[{oi}] property[{pi}] docstring"))
            pf = r.u32(f"object[{oi}] property[{pi}] user flags")
            pflags = r.u8(f"object[{oi}] property[{pi}] flags")
            if pflags & ~0x07:
                r.fail(f"object[{oi}] property[{pi}] unknown flags 0x{pflags:02x}")
            pfunctions: list[Function] = []
            auto_var: str | None = None
            if pflags & 0x04:
                auto_var = s(string_index(f"object[{oi}] property[{pi}] auto var"))
            else:
                if pflags & 0x01:
                    pfunctions.append(function(f"object[{oi}] property[{pi}] getter",
                                               f"get_{name}", object_name, "", "getter"))
                if pflags & 0x02:
                    pfunctions.append(function(f"object[{oi}] property[{pi}] setter",
                                               f"set_{name}", object_name, "", "setter"))
            properties.append(Property(name, type_name, pdoc, pf, pflags, auto_var, pfunctions))

        state_count = r.count(f"object[{oi}] state count")
        states: list[State] = []
        for si in range(state_count):
            state_name = s(string_index(f"object[{oi}] state[{si}] name"))
            fn_count = r.count(f"object[{oi}] state[{si}] function count")
            fns = []
            for fi in range(fn_count):
                name = s(string_index(f"object[{oi}] state[{si}] function[{fi}] name"))
                fns.append(function(f"object[{oi}] state[{si}] function[{fi}]", name,
                                    object_name, state_name, "method"))
            states.append(State(state_name, fns))
        objects.append(Object(object_name, size_bytes, parent_class, docstring, obj_user_flags,
                              auto_state_name, variables, properties, states))

    trailing = len(data) - r.pos
    warnings: list[str] = []
    if minor != 2:
        warnings.append(f"PEX 3.{minor} is not the observed Skyrim 3.2 layout")
    if trailing:
        warnings.append(f"{trailing} trailing byte(s) after object table")
    for fn in (f for o in objects for f in o.functions):
        if fn.role != "method":
            debug_type = 1 if fn.role == "getter" else 2
        else:
            debug_type = 0
        debug_name = fn.name
        if fn.role == "getter" and debug_name.startswith("get_"):
            debug_name = debug_name[4:]
        elif fn.role == "setter" and debug_name.startswith("set_"):
            debug_name = debug_name[4:]
        matches = [d for d in debug_functions
                   if d.object_name == fn.object_name and d.state_name == fn.state_name
                   and d.function_name == debug_name and d.function_type == debug_type]
        if matches:
            fn.debug_lines = matches[0].line_numbers
            if fn.debug_lines and len(fn.debug_lines) != len(fn.instructions):
                warnings.append(
                    f"{fn.qualified_name}: debug line count {len(fn.debug_lines)} != "
                    f"instruction count {len(fn.instructions)}")
        elif debug_present:
            warnings.append(f"{fn.qualified_name}: no matching debug function record")
        for ins in fn.instructions:
            if ins.opcode > SKYRIM_MAX_OPCODE:
                warnings.append(f"{fn.qualified_name}+0x{ins.offset:x}: opcode "
                                f"0x{ins.opcode:02x} is outside Skyrim 3.2's 0x00..0x24 range")

    return Pex(
        path=path,
        sha256=hashlib.sha256(data).hexdigest(),
        file_bytes=len(data),
        major_version=major,
        minor_version=minor,
        game_id=game_id,
        compilation_time=compilation_time,
        source_name=source_name,
        user_name=user_name,
        computer_name=computer_name,
        strings=strings,
        debug_present=debug_present,
        debug_modification_time=debug_time,
        debug_functions=debug_functions,
        user_flags=user_flags,
        objects=objects,
        warnings=warnings,
        trailing_bytes=trailing,
    )


def load_pex(path: Path) -> Pex:
    return parse_pex(path.read_bytes(), str(path))


def print_text(pex: Pex, show_strings: bool, show_functions: bool,
               show_instructions: bool, string_limit: int) -> None:
    s = pex.summary()
    print(f"file: {pex.path}")
    print(f"sha256: {pex.sha256}")
    print(f"format: PEX {pex.major_version}.{pex.minor_version}, magic 0xDEC057FA, "
          f"big-endian, game_id {pex.game_id}, {pex.file_bytes} bytes")
    print(f"source: {pex.source_name!r} compiled_by={pex.user_name!r} "
          f"computer={pex.computer_name!r} unix_time={pex.compilation_time}")
    print(f"strings: {len(pex.strings)}; debug: {len(pex.debug_functions)} functions; "
          f"user_flags: {len(pex.user_flags)}")
    print(f"objects: {len(pex.objects)}; states: {s['state_count']}; "
          f"properties: {s['property_count']}; variables: {s['variable_count']}; "
          f"functions: {len(pex.functions)}; instructions: {s['instruction_count']}; "
          f"code_bytes: {s['code_bytes']}")
    for obj in pex.objects:
        print(f"  object {obj.name} parent={obj.parent_class!r} auto_state={obj.auto_state_name!r} "
              f"functions={len(obj.functions)}")
    print("opcode histogram:")
    for op, count in sorted(pex.opcode_histogram.items()):
        name = OPCODES[op][0] if op < len(OPCODES) else "unknown"
        print(f"  0x{op:02x} {name:<28} {count}")
    if show_strings:
        print("string table:")
        limit = len(pex.strings) if string_limit < 0 else min(string_limit, len(pex.strings))
        for i, text in enumerate(pex.strings[:limit]):
            print(f"  [{i}] {text!r}")
        if limit < len(pex.strings):
            print(f"  ... {len(pex.strings) - limit} omitted (use --string-limit -1)")
    if show_functions or show_instructions:
        print("functions:")
        for fn in pex.functions:
            print(f"  {fn.qualified_name} ({fn.role}) return={fn.return_type} "
                  f"flags=0x{fn.flags:02x} params={len(fn.params)} locals={len(fn.locals)} "
                  f"instructions={len(fn.instructions)} bytes={sum(i.byte_size for i in fn.instructions)}")
            if show_instructions:
                for ins in fn.instructions:
                    print(f"    {ins.display(pex.strings)}")
    if pex.warnings:
        print("warnings:")
        for warning in pex.warnings:
            print(f"  {warning}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--json", action="store_true", help="emit a JSON array (one object per file)")
    ap.add_argument("--strings", action="store_true", help="include the complete string table")
    ap.add_argument("--string-limit", type=int, default=20,
                    help="text-mode string listing limit; -1 means all (default: 20)")
    ap.add_argument("--functions", action="store_true", help="list functions and code sizes")
    ap.add_argument("--instructions", action="store_true", help="decode every instruction")
    args = ap.parse_args(argv)

    parsed: list[Pex] = []
    failures = 0
    for path in args.files:
        try:
            parsed.append(load_pex(path))
        except (OSError, PexError, struct.error) as exc:
            failures += 1
            if args.json:
                parsed.append({"path": str(path), "error": str(exc)})
            else:
                print(f"ERROR {path}: {exc}", file=sys.stderr)
    if args.json:
        payload = [p.summary(args.strings, args.functions, args.instructions)
                   if isinstance(p, Pex) else p for p in parsed]
        print(json.dumps(payload, indent=2, ensure_ascii=False))
    else:
        for index, pex in enumerate(parsed):
            if index:
                print()
            print_text(pex, args.strings, args.functions, args.instructions, args.string_limit)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
