"""Lift a bounded, self-contained D3DX matrix routine from a locally owned DLL.

No game code is included or uploaded. Requires capstone and pefile, and the
staged upstream lift.py. Rejects indirect control transfers and failed lifts.
"""
import argparse
from collections import deque
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys

BASE = 0x08000000
CANDIDATES = {"D3DXMatrixTranspose": 1, "D3DXMatrixScaling": 2,
              "D3DXMatrixTranslation": 3, "D3DXMatrixMultiply": 4,
              "D3DXMatrixMultiplyTranspose": 5,
              "D3DXVec3TransformCoord": 6, "D3DXVec3TransformNormal": 7,
              "D3DXVec3Normalize": 8, "D3DXMatrixInverse": 9,
              "D3DXPlaneNormalize": 10, "D3DXPlaneTransform": 11}
MAX_FUNCTIONS, MAX_INSNS = 32, 2048
HOTPATCH_JMP = b"\xff\x25"
HOTPATCH_PAD = b"\xcc" * 5


def u32(image, base, addr):
    off = addr - base
    if off < 0 or off + 4 > len(image):
        return None
    return int.from_bytes(image[off:off + 4], "little")


def resolve_ilt_thunk(image, base, root):
    """Resolve the MSVC lazy incremental-link thunk used by D3DX9_42.dll.

    The DLL routes exports through a writable slot that a one-time initializer
    patches from a guard wrapper to the real body.  Only the exact, verifiable
    layout is accepted:

        root+0   : FF 25 <slot>        jmp dword ptr [slot]
        root+6   : CC CC CC CC CC      int3 padding
        root+11  : 8B FF 55 8B ...     real function body
        slot     : wrapper (file-initialized value)
        wrapper  : 8B FF 55 8B EC, push imm8, call <init>
        wrapper+0x13..0x17 : int3 padding
        wrapper+0x18       : FF 25 <slot>   (same thunk as the export)
        wrapper+0x23       : real body == root+11

    Returns a dict describing the chain, or None when the export is already a
    plain function.  Raises ValueError when a thunk-like export does not match
    the verified layout, so a malformed DLL cannot silently slip through.
    The initializer is located but not analysed; prepare() must prove that the
    lifted slice never enters the wrapper or initializer before calling the
    bypass explicit.
    """
    off = root - base
    if off < 5 or off + 11 > len(image):
        return None
    if image[off:off + 2] != HOTPATCH_JMP:
        return None
    slot = int.from_bytes(image[off + 2:off + 6], "little")
    if image[off + 6:off + 11] != HOTPATCH_PAD:
        raise ValueError(f"export {root:#x}: hotpatch thunk without int3 padding")
    if not 0 <= slot - base <= len(image) - 4:
        raise ValueError(f"export {root:#x}: slot {slot:#x} outside image")
    wrapper = u32(image, base, slot)
    if wrapper is None:
        raise ValueError(f"export {root:#x}: unreadable slot {slot:#x}")
    wo = wrapper - base
    if wo < 0 or wo >= off:
        raise ValueError(f"export {root:#x}: wrapper {wrapper:#x} is not before the export thunk")
    if image[wo:wo + 5] != b"\x8b\xff\x55\x8b\xec":
        raise ValueError(f"export {root:#x}: wrapper {wrapper:#x} lacks frame prologue")
    if image[wo + 5:wo + 7] != b"\x6a\x01" or image[wo + 7] != 0xe8:
        raise ValueError(f"export {root:#x}: wrapper does not call a one-time initializer")
    rel = int.from_bytes(image[wo + 8:wo + 12], "little", signed=True)
    init = (wrapper + 12 + rel) & 0xffffffff
    if not 0 <= init - base < len(image):
        raise ValueError(f"export {root:#x}: initializer {init:#x} outside image")
    thunk = HOTPATCH_JMP + slot.to_bytes(4, "little")
    call_slot = b"\xff\x15" + slot.to_bytes(4, "little")
    if thunk not in image[wo + 12:off] and call_slot not in image[wo + 12:off]:
        raise ValueError(f"export {root:#x}: wrapper does not forward through slot {slot:#x}")
    if image[off - 5:off] != HOTPATCH_PAD:
        raise ValueError(f"export {root:#x}: wrapper padding before the export thunk does not match")
    real = root + 11
    if image[real - base:real - base + 4] != b"\x8b\xff\x55\x8b":
        raise ValueError(f"export {root:#x}: real body lacks MSVC prologue")
    return {"slot": slot, "wrapper": wrapper, "init": init, "real": real}


def fnv64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def discover(md, image, base, executable, root, x86):
    """A small bounded direct-control-flow slice, in upstream JSON format."""
    def code_at(addr):
        return any(lo <= addr < hi for lo, hi in executable)

    def decode(addr):
        if not code_at(addr) or not 0 <= addr-base < len(image):
            raise ValueError(f"control leaves executable section at {addr:#x}")
        i = next(md.disasm(image[addr-base:addr-base+15], addr, 1), None)
        if i is None or not code_at(addr + i.size - 1):
            raise ValueError(f"cannot decode {addr:#x}")
        return i

    pending = deque([root])
    funcs = {}
    edges = {}
    total = 0
    while pending:
        start = pending.popleft()
        if start in funcs:
            continue
        if len(funcs) >= MAX_FUNCTIONS:
            raise ValueError("direct-call closure exceeds function budget")
        leaders, seen = {start}, {}
        edges[start] = set()
        work = deque([start])
        while work:
            a = work.popleft()
            while a not in seen:
                i = decode(a)
                total += 1
                if total > MAX_INSNS:
                    raise ValueError("instruction budget exceeded")
                if i.mnemonic.startswith("loop") or i.mnemonic in {"jecxz", "jcxz", "int", "int3", "hlt", "ud2", "retf", "iret", "syscall", "sysenter"}:
                    raise ValueError(f"unsupported control instruction {i.mnemonic}")
                seen[a] = i.size
                nxt = a + i.size
                groups = set(i.groups)
                if i.id == x86.X86_INS_CALL:
                    if len(i.operands) != 1 or i.operands[0].type != x86.X86_OP_IMM:
                        raise ValueError(f"indirect call at {a:#x}; pilot needs a closed direct-call slice")
                    dst = i.operands[0].imm & 0xffffffff
                    if not code_at(dst):
                        raise ValueError(f"external direct call at {a:#x}")
                    pending.append(dst)
                    edges[start].add(dst)
                elif x86.X86_GRP_RET in groups:
                    break
                elif x86.X86_GRP_JUMP in groups:
                    if not i.operands or i.operands[0].type != x86.X86_OP_IMM:
                        raise ValueError(f"indirect branch at {a:#x}")
                    dst = i.operands[0].imm & 0xffffffff
                    targets = [dst] if i.id == x86.X86_INS_JMP else [dst, nxt]
                    for t in targets:
                        if not code_at(t):
                            raise ValueError(f"branch outside image at {a:#x}")
                        if t not in leaders:
                            leaders.add(t)
                            work.append(t)
                    break
                a = nxt
                if a in leaders:
                    work.append(a)
                    break
        # Reject mid-instruction branch targets / overlapping decodes.
        ordered = sorted(seen)
        blocks = []
        for a in ordered:
            end = a+seen[a]
            if blocks and a < blocks[-1][1]:
                raise ValueError("overlapping instruction decode")
            if not blocks or a in leaders or a != blocks[-1][1]:
                blocks.append([a, end])
            else:
                blocks[-1][1] = end
        if not blocks or any(t not in seen for t in leaders):
            raise ValueError("unresolved basic block")
        funcs[start] = blocks
    active, finished = set(), set()
    def visit(a):
        if a in active:
            raise ValueError("recursive direct-call closure rejected")
        if a in finished:
            return
        active.add(a)
        for b in edges[a]:
            visit(b)
        active.remove(a);finished.add(a)
    visit(root)
    return {"image_base": base, "entry": root,
            "functions": {hex(a): b for a, b in sorted(funcs.items())},
            "iat": {}, "jump_tables": {}, "indirect_jumps": []}, total


def prepare(dll, staged, out, requested):
    try:
        import capstone
        import pefile
        from capstone import x86
    except ImportError as exc:
        raise ValueError("Install capstone and pefile first: python -m pip install capstone==5.0.9 pefile==2024.8.26") from exc
    raw = dll.read_bytes()
    pe = pefile.PE(data=raw)
    if pe.FILE_HEADER.Machine != 0x14c or pe.OPTIONAL_HEADER.Magic != 0x10b:
        raise ValueError("Expected the 32-bit x86 d3dx9_42.dll")
    original = pe.OPTIONAL_HEADER.ImageBase
    if original != BASE and not getattr(pe, "DIRECTORY_ENTRY_BASERELOC", None):
        raise ValueError("DLL has no relocation directory for the planned base")
    if original != BASE:
        pe.relocate_image(BASE)
        pe.OPTIONAL_HEADER.ImageBase = BASE
    image = bytes(pe.get_memory_mapped_image(ImageBase=BASE))
    code = [(BASE+s.VirtualAddress, BASE+s.VirtualAddress+max(s.Misc_VirtualSize, s.SizeOfRawData))
            for s in pe.sections if s.Characteristics & 0x20000000]
    exports = {s.name.decode("ascii", "replace"): BASE+s.address
               for s in getattr(getattr(pe,"DIRECTORY_ENTRY_EXPORT",None),"symbols",[])
               if s.name and not s.forwarder}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    if not (staged/"lift.py").is_file():
        raise ValueError("Missing staged/lift.py; run stage_upstream.py first")
    out.mkdir(parents=True, exist_ok=False)
    reasons = {}
    env = dict(os.environ, LIFT_GAME="skyrim", LIFT_FILES="1", LIFT_TSO="0", LIFT_MEMORDER="0")
    for name in [requested] if requested else CANDIDATES:
        try:
            if name not in exports:
                raise ValueError("export not present")
            entry = exports[name]
            thunk = resolve_ilt_thunk(image, BASE, entry)
            start = thunk["real"] if thunk else entry
            analysis, insns = discover(md, image, BASE, code, start, x86)
            if thunk:
                ranges = [tuple(b) for blocks in analysis["functions"].values() for b in blocks]
                starts = {int(a, 16) for a in analysis["functions"]}
                if thunk["init"] in starts or any(lo <= thunk["init"] < hi for lo, hi in ranges):
                    raise ValueError("real body reaches the lazy initializer; the bypass claim would be false")
                if any(lo < entry and hi > thunk["wrapper"] for lo, hi in ranges):
                    raise ValueError("real body reaches the guard wrapper; the bypass claim would be false")
            attempt = out/name
            attempt.mkdir()
            info = attempt/"functions.json"
            info.write_text(json.dumps(analysis,indent=2), encoding="utf-8")
            gen = attempt/"gen"
            result = subprocess.run([sys.executable,str(staged/"lift.py"),str(dll),str(info),str(gen),"pilot",f"{BASE:x}"],env=env,capture_output=True,text=True)
            (attempt/"lift.log").write_text(result.stdout+result.stderr,encoding="utf-8")
            if result.returncode:
                raise ValueError(f"lifter failed; see {attempt/'lift.log'}")
            rejected = json.loads((gen/"pilot_rejected.json").read_text(encoding="utf-8"))
            if rejected:
                raise ValueError(f"{len(rejected)} functions rejected by upstream")
            bodies = list(gen.glob("pilot_[0-9]*.c"))
            if not bodies or any("rt_unimpl(" in p.read_text(encoding="utf-8") or "rt_int3(" in p.read_text(encoding="utf-8") for p in bodies):
                raise ValueError("generated body contains unsupported instruction trap")
            # Instrument the C output, not game bytes, so loops stop with a guest fault.
            labels = 0
            for p in bodies:
                body=p.read_text(encoding="utf-8")
                body,n=re.subn(r"^(L_[A-Za-z0-9]+:)[ \t]*$",r"\1\n    n3ds_pilot_tick();",body,flags=re.MULTILINE)
                labels+=n;p.write_text('#include "pilot_guard.h"\n'+body,encoding="utf-8")
            if not labels:
                raise ValueError("no generated block labels for execution guard")
            manifest = {"export":name,"case":CANDIDATES[name],"export_address":entry,"address":start,"base":BASE,"file_size":len(raw),
                        "fnv64":f"{fnv64(raw):016x}","sha256":hashlib.sha256(raw).hexdigest(),"functions":len(analysis["functions"]),"instructions":insns,
                        "ilt_thunk":thunk,"lazy_ilt_init_bypassed":bool(thunk)}
            (gen/"pilot_manifest.json").write_text(json.dumps(manifest,indent=2),encoding="utf-8")
            ilt_wrapper = thunk["wrapper"] if thunk else 0
            ilt_init = thunk["init"] if thunk else 0
            (gen/"pilot_config.h").write_text(f'''#pragma once
#define PILOT_EXPORT "{name}"
#define PILOT_CASE {CANDIDATES[name]}u
#define PILOT_ENTRY 0x{start:08x}u
#define PILOT_BASE 0x{BASE:08x}u
#define PILOT_FILE_BYTES UINT64_C({len(raw)})
#define PILOT_FNV64 UINT64_C(0x{fnv64(raw):016x})
#define PILOT_ILT_BYPASS {1 if thunk else 0}u
#define PILOT_ILT_WRAPPER 0x{ilt_wrapper:08x}u
#define PILOT_ILT_INIT 0x{ilt_init:08x}u
''',encoding="utf-8")
            print(json.dumps(manifest,indent=2));print("Use PILOT_GEN="+str(gen));return
        except (ValueError,OSError,KeyError,json.JSONDecodeError) as exc:
            reasons[name]=str(exc);print(f"{name}: {exc}",file=sys.stderr)
    (out/"rejections.json").write_text(json.dumps(reasons,indent=2),encoding="utf-8")
    raise ValueError("No self-contained matrix routine passed the conservative slice gate")


if __name__ == "__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("dll",type=Path);p.add_argument("staged",type=Path);p.add_argument("output",type=Path)
    p.add_argument("--export",choices=CANDIDATES)
    a=p.parse_args()
    try:prepare(a.dll.resolve(),a.staged.resolve(),a.output.resolve(),a.export)
    except (ValueError,OSError) as e:sys.exit(str(e))
