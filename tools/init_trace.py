#!/usr/bin/env python3
"""Read-only CRT / C++ initializer and D3D9 entry tracer for TESV.exe.

This tool exists because the startup frontier is dominated by function-pointer
tables, not by ordinary call edges.  It does not modify the executable, the
generated slices, the growth scripts or the runtime.  Everything it prints is
either decoded from the PE image or read from a slice/report file; the origin
is marked explicitly.

Subcommands
-----------
  pe        PE entry, TLS directory, load config, section map.
  tables    Discover CRT init/terminator tables from the actual loop code.
  list      Dump one table (base:end) with slice membership and target class.
  trace     Bounded forward direct-call trace (entry -> WinMain, etc.).
  frontier  Remaining table walk + first missing callee (growth frontier).
  d3d9      Dynamic Direct3DCreate9 resolution, call sites and factory uses.
  slots     Heuristic COM vtable slot calls in bounded function ranges.
  run       Annotate a saved host run (probe log [+ profile csv]) against a slice.
            tools/annotate_host_run.py produces the readable Markdown report.

Typical use
-----------
  python tools/init_trace.py pe E:\\skyr\\TESV.exe
  python tools/init_trace.py tables E:\\skyr\\TESV.exe ^
      --functions E:\\Skyrim_N3DS_build\\analysis\\tesv\\functions.json ^
      --slice E:\\Skyrim_N3DS_build\\analysis\\growth21\\iter04\\slice_functions.json
  python tools/init_trace.py frontier E:\\skyr\\TESV.exe ^
      --table 0x106b464:0x1076a98 --after 0x107007c ^
      --slice E:\\Skyrim_N3DS_build\\analysis\\growth21\\iter04\\slice_functions.json
  python tools/init_trace.py d3d9 E:\\skyr\\TESV.exe
  python tools/init_trace.py run E:\\Skyrim_N3DS_build\\analysis\\growth21\\iter04 ^
      --slice E:\\Skyrim_N3DS_build\\analysis\\growth21\\iter04\\slice_functions.json

Dependencies: pefile, capstone (the project .venv has both).
"""

import argparse
import collections
import json
import struct
import sys
from pathlib import Path

try:
    import pefile
    import capstone
except ImportError as exc:  # pragma: no cover - operator environment issue
    sys.exit("init_trace.py needs pefile and capstone in the active Python: %s" % exc)


# --------------------------------------------------------------------------- #
# PE / image helpers
# --------------------------------------------------------------------------- #

class Image:
    def __init__(self, path):
        self.path = Path(path)
        self.pe = pefile.PE(str(self.path))
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = False
        self.sections = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\x00").decode("ascii", "replace")
            self.sections.append({
                "name": name,
                "va": self.base + s.VirtualAddress,
                "end": self.base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData),
                "raw": s.PointerToRawData,
                "raw_size": s.SizeOfRawData,
                "exec": bool(s.Characteristics & 0x20000000),
            })
        self.iat = {}
        for entry in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = entry.dll.decode()
            for imp in entry.imports:
                name = imp.name.decode() if imp.name else "#%d" % imp.ordinal
                self.iat[imp.address] = dll + "!" + name

    def u32(self, va):
        off = va - self.base
        if off < 0 or off + 4 > len(self.img):
            return None
        return struct.unpack_from("<I", self.img, off)[0]

    def i32(self, va):
        off = va - self.base
        if off < 0 or off + 4 > len(self.img):
            return None
        return struct.unpack_from("<i", self.img, off)[0]

    def region(self, va):
        for s in self.sections:
            if s["va"] <= va < s["end"]:
                return s["name"]
        return "outside"

    def is_text(self, va):
        return self.region(va) == ".text"

    def disasm(self, va, count=64):
        off = va - self.base
        if off < 0 or off >= len(self.img):
            return []
        out = []
        for ins in self.md.disasm(self.img[off:off + count * 16], va):
            out.append(ins)
            if len(out) >= count:
                break
        return out

    def disasm_upto(self, va, end, max_insns=96):
        """Disassemble only inside [va,end) so a short function cannot bleed
        into its neighbour (the CRT loops are adjacent to each other)."""
        off = va - self.base
        if off < 0 or off >= len(self.img) or end <= va:
            return []
        body = self.img[off:off + min(end - va + 16, 4096)]
        out = []
        for ins in self.md.disasm(body, va):
            if ins.address >= end:
                break
            out.append(ins)
            if len(out) >= max_insns:
                break
        return out

    def containing_function(self, va, starts):
        # starts is a sorted list of discovered function entry addresses.
        import bisect
        i = bisect.bisect_right(starts, va) - 1
        if i < 0:
            return None
        return starts[i]

    def direct_calls(self, start, end, limit=20000):
        """Yield (call_site, target) for direct E8 calls inside [start,end)."""
        off = start - self.base
        body = self.img[off:off + max(0, end - start)]
        out = []
        for i in range(0, len(body) - 4):
            if body[i] == 0xE8:
                rel = struct.unpack_from("<i", body, i + 1)[0]
                tgt = start + i + 5 + rel
                if self.base <= tgt < self.base + 0x2000000:
                    out.append((start + i, tgt))
                    if len(out) >= limit:
                        break
        return out

    def dword_refs(self, value):
        """All VAs in the image whose little-endian dword equals value."""
        pat = struct.pack("<I", value)
        out = []
        pos = 0
        while True:
            i = self.img.find(pat, pos)
            if i < 0:
                break
            out.append(self.base + i)
            pos = i + 1
        return out


def load_starts(functions_path):
    if not functions_path:
        return [], {}
    data = json.loads(Path(functions_path).read_text(encoding="utf-8"))
    funcs = data.get("functions", data)
    starts = sorted(int(k, 16) for k in funcs)
    return starts, funcs


def load_slice(path):
    if not path:
        return set()
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    funcs = data.get("functions", data)
    if isinstance(funcs, dict):
        return set(int(k, 16) for k in funcs)
    if isinstance(funcs, list):
        out = set()
        for item in funcs:
            if isinstance(item, str):
                out.add(int(item, 16))
            elif isinstance(item, int):
                out.add(item)
        return out
    return set()


# --------------------------------------------------------------------------- #
# CRT loop / table discovery
# --------------------------------------------------------------------------- #

def loop_kind(img, addr, end=None):
    """Classify an _initterm-style loop function.

    Returns (kind, stops_on_error) where kind is
      'rel2'  - two stack arguments: [ebp+8]=begin, [ebp+0xc]=end
      'rel1'  - begin in a register (eax/ecx), end in [ebp+8]
      'abs'   - both table bounds are immediates inside the loop body
    or None if the body does not match.
    """
    ins = img.disasm_upto(addr, end or addr + 0x80, 48)
    if not ins:
        return None
    text = " ".join("%s %s" % (i.mnemonic, i.op_str) for i in ins)
    stops = "test eax, eax" in text and "jne" in text
    # Two-stack-arg loop (_initterm_e style, 0xf5897a).
    if "call ecx" in text and "cmp esi, dword ptr [ebp + 0xc]" in text:
        return ("rel2", stops)
    # One-stack-arg loop (plain _initterm style, 0xf5895d).
    if ("call eax" in text or "call ecx" in text) and \
       "cmp esi, dword ptr [ebp + 8]" in text and "add esi, 4" in text:
        return ("rel1", False)
    # Fully linked loop with absolute bounds (0xf69089 / 0xf690af).
    if "call eax" in text and "cmp edi, esi" in text:
        return ("abs", False)
    return None


def loop_arg_reg(img, addr, end=None):
    """Register holding the begin pointer for a 'rel1' loop (0xf5895d: eax)."""
    for ins in img.disasm_upto(addr, end or addr + 0x40, 12):
        if ins.mnemonic == "mov" and ins.op_str.startswith("esi, "):
            return ins.op_str.split(", ", 1)[1]
    return "eax"


def find_loop_functions(img, starts, funcs=None):
    """Find the _initterm loops by exact emitter signatures.

    The two relative-argument loops and the two absolute-range loops have
    stable prologues in this CRT build.  Matching those bytes avoids scanning
    thousands of ordinary functions that merely contain a similar instruction.
    """
    rel2_sig = bytes.fromhex("8bff558bec568b750833c0eb")
    rel1_sig = bytes.fromhex("8bff558bec568bf0eb")
    abs_head = bytes.fromhex("8bff56b8")
    abs_mid = bytes.fromhex("578bf83bc6")
    out = []
    for a in starts:
        off = a - img.base
        head = img.img[off:off + 24]
        if not head:
            continue
        is_rel2 = head.startswith(rel2_sig)
        is_rel1 = head.startswith(rel1_sig)
        is_abs = head.startswith(abs_head) and abs_mid in head
        if not (is_rel2 or is_rel1 or is_abs):
            continue
        end = function_end(funcs, a) if funcs else None
        k = loop_kind(img, a, end)
        if k:
            out.append((a, k[0], k[1]))
    return out


def function_end(funcs, addr):
    """Manifest function end (max block end); falls back to addr+0x80."""
    if not funcs:
        return addr + 0x80
    rng = funcs.get("0x%x" % addr) or funcs.get(hex(addr))
    if not rng:
        return addr + 0x80
    try:
        return max(int(e) for _, e in rng)
    except Exception:
        return addr + 0x80


def absolute_bounds(img, addr, end=None):
    """Extract the two table bounds from an absolute-range loop body."""
    vals = []
    for ins in img.disasm_upto(addr, end or addr + 0x80, 24):
        if ins.mnemonic in ("mov", "cmp") and ins.op_str:
            parts = ins.op_str.split(", ")
            if len(parts) == 2 and parts[1].startswith("0x"):
                vals.append(int(parts[1], 16))
    # The loop loads start then end; the first two distinct immediates inside
    # the image are the table pair.
    cand = [v for v in vals if img.base <= v < img.base + 0x2000000]
    if len(cand) >= 2:
        a, b = cand[0], cand[1]
        return (min(a, b), max(a, b))
    return None


def valid_table_range(img, lo, hi, allow_empty=False):
    """Reject stray address pairs that are not a plausible pointer table."""
    if not (img.base <= lo <= hi <= img.base + 0x2000000):
        return False
    if allow_empty and lo == hi:
        return True
    if lo == hi:
        return False
    if (hi - lo) % 4 != 0:
        return False
    if (hi - lo) // 4 > 20000:
        return False
    return img.region(lo) in (".rdata", ".data", ".tls") and \
        img.region(hi - 1) in (".rdata", ".data", ".tls")


def table_args_for_call(img, call_site, func_start, kind, arg_reg):
    """Recover the two table bounds passed to a relative _initterm loop.

    The CRT build emits either two `push imm32` instructions or, for the
    one-stack-argument loop, `mov eax, begin` plus `mov dword ptr [esp], end`.
    Scanning the 64 bytes before the call is exact for these emitters and keeps
    discovery independent of the function manifest.
    """
    off = call_site - img.base
    win = img.img[max(0, off - 64):off]
    base_va = call_site - len(win)
    stack = []
    reg = []
    for i in range(len(win)):
        if win[i] == 0x68 and i + 5 <= len(win):
            stack.append((base_va + i, struct.unpack_from("<I", win, i + 1)[0]))
        elif win[i] == 0xC7 and win[i:i + 3] == b"\xc7\x04\x24" and i + 7 <= len(win):
            stack.append((base_va + i, struct.unpack_from("<I", win, i + 3)[0]))
        elif win[i] == 0xB8 and i + 5 <= len(win):
            reg.append((base_va + i, struct.unpack_from("<I", win, i + 1)[0]))
    if kind == "rel2":
        if len(stack) >= 2:
            return stack[-2][1], stack[-1][1]
        return None
    if kind == "rel1":
        if not stack or not reg:
            return None
        return reg[-1][1], stack[-1][1]
    return None


def discover_tables(img, functions_path=None):
    """Return discovered initializer tables as dicts."""
    starts, funcs = load_starts(functions_path)
    if not starts:
        # Fall back to a bounded scan of the executable section for prologues.
        text = next(s for s in img.sections if s["name"] == ".text")
        starts = [text["va"]]
    loops = find_loop_functions(img, starts, funcs) if functions_path else []

    tables = []
    seen = set()
    for loop_addr, kind, stops in loops:
        fend = function_end(funcs, loop_addr)
        if kind == "abs":
            bounds = absolute_bounds(img, loop_addr, fend)
            if not bounds:
                continue
            lo, hi = bounds
            if not valid_table_range(img, lo, hi, allow_empty=True):
                continue
            key = (loop_addr, lo, hi)
            if key in seen:
                continue
            seen.add(key)
            tables.append({
                "kind": "initterm_absolute",
                "loop": loop_addr,
                "base": lo,
                "end": hi,
                "stops_on_error": stops,
                "slots": (hi - lo) // 4,
                "source": "immediates inside loop body",
            })
            continue
        # relative loop: find call sites; recover the arguments in the caller
        reg = loop_arg_reg(img, loop_addr, fend)
        for site in range_scan_call_sites(img, loop_addr):
            fstart = img.containing_function(site, starts) if functions_path else max(
                img.base, site - 512)
            args = table_args_for_call(img, site, fstart, kind, reg)
            if not args:
                continue
            lo, hi = min(args), max(args)
            if not valid_table_range(img, lo, hi):
                continue
            key = (loop_addr, lo, hi)
            if key in seen:
                continue
            seen.add(key)
            tables.append({
                "kind": "initterm_e" if stops else "initterm",
                "loop": loop_addr,
                "call_site": site,
                "caller": fstart,
                "base": lo,
                "end": hi,
                "stops_on_error": stops,
                "slots": (hi - lo) // 4,
                "source": "arguments before call",
            })
    tables.sort(key=lambda t: (t["base"], t["end"], t["loop"]))
    return tables


def range_scan_call_sites(img, target, max_hits=64):
    """Find E8 call sites to target across the executable sections."""
    out = []
    for s in img.sections:
        if not s["exec"]:
            continue
        off = s["va"] - img.base
        body = img.img[off:off + (s["end"] - s["va"])]
        p = 0
        while len(out) < max_hits:
            i = body.find(b"\xe8", p)
            if i < 0:
                break
            p = i + 1
            if i + 5 > len(body):
                break
            rel = struct.unpack_from("<i", body, i + 1)[0]
            if s["va"] + i + 5 + rel == target:
                out.append(s["va"] + i)
                if len(out) >= max_hits:
                    break
    return out


# --------------------------------------------------------------------------- #
# Target classification
# --------------------------------------------------------------------------- #

def classify_target(img, va):
    """Classify a table target by its first bytes (documented heuristic)."""
    off = va - img.base
    b = img.img[off:off + 16]
    if len(b) < 6:
        return "outside"
    if b[0] == 0x68 and b[5] == 0xE8:
        imm = struct.unpack_from("<I", b, 1)[0]
        rel = struct.unpack_from("<i", b, 6)[0]
        if va + 10 + rel == 0xF520BC:
            return "atexit_thunk"
        if img.region(imm).endswith("text"):
            return "atexit_thunk_text_arg"
    if b[0] == 0xF6 and b[1] == 0x05:
        return "static_guard"
    if b[0] in (0xA1, 0xA3, 0xD9, 0xDD):
        return "data_init"
    if b[0] == 0xE9 or b[0] == 0xE8:
        return "jump_thunk"
    return "code"


def table_rows(img, base, end, starts, slset):
    rows = []
    for slot in range(base, end, 4):
        v = img.u32(slot)
        if not v:
            rows.append({"slot": slot, "index": (slot - base) // 4, "target": 0,
                         "null": True})
            continue
        rows.append({
            "slot": slot,
            "index": (slot - base) // 4,
            "target": v,
            "null": False,
            "region": img.region(v),
            "is_function_start": v in starts if starts else None,
            "in_slice": v in slset,
            "class": classify_target(img, v),
        })
    return rows


# --------------------------------------------------------------------------- #
# Subcommands
# --------------------------------------------------------------------------- #

def cmd_pe(args):
    img = Image(args.exe)
    entry = img.base + img.pe.OPTIONAL_HEADER.AddressOfEntryPoint
    out = {
        "exe": str(img.path),
        "image_base": img.base,
        "entry": entry,
        "size_of_image": img.pe.OPTIONAL_HEADER.SizeOfImage,
        "sections": img.sections,
    }
    tls = getattr(img.pe, "DIRECTORY_ENTRY_TLS", None)
    if tls:
        st = tls.struct
        out["tls"] = {
            "start_raw": st.StartAddressOfRawData,
            "end_raw": st.EndAddressOfRawData,
            "index": st.AddressOfIndex,
            "callbacks": st.AddressOfCallBacks,
            "zero_fill": st.SizeOfZeroFill,
            "characteristics": st.Characteristics,
        }
        cbs = []
        if st.AddressOfCallBacks:
            for i in range(16):
                v = img.u32(st.AddressOfCallBacks + i * 4)
                if v is None:
                    break
                cbs.append(v)
                if v == 0 and i > 0 and img.u32(st.AddressOfCallBacks + (i + 1) * 4) == 0:
                    break
        out["tls"]["callback_array"] = cbs
    lc = getattr(img.pe, "DIRECTORY_ENTRY_LOAD_CONFIG", None)
    if lc:
        out["load_config"] = {
            "va": img.base + img.pe.OPTIONAL_HEADER.DATA_DIRECTORY[
                pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG"]].VirtualAddress,
            "size": lc.struct.Size,
            "security_cookie": getattr(lc.struct, "SecurityCookie", None),
            "seh_table": getattr(lc.struct, "SEHandlerTable", None),
            "seh_count": getattr(lc.struct, "SEHandlerCount", None),
        }
    print(json.dumps(out, indent=2))


def cmd_tables(args):
    img = Image(args.exe)
    starts, _ = load_starts(args.functions)
    slset = load_slice(args.slice)
    tables = discover_tables(img, args.functions)
    report = {"exe": str(img.path), "slice": args.slice, "tables": []}
    for t in tables:
        rows = table_rows(img, t["base"], t["end"], starts, slset)
        nz = [r for r in rows if not r["null"]]
        bad = [r for r in nz if starts and not r["is_function_start"]]
        t = dict(t)
        t["non_null"] = len(nz)
        t["null"] = len(rows) - len(nz)
        t["in_slice"] = sum(1 for r in nz if r["in_slice"])
        t["not_function_start"] = len(bad)
        t["classes"] = dict(collections.Counter(r["class"] for r in nz))
        t["first"] = [{k: r[k] for k in ("index", "slot", "target", "in_slice", "class")}
                      for r in nz[:8]]
        t["last"] = [{k: r[k] for k in ("index", "slot", "target", "in_slice", "class")}
                     for r in nz[-4:]]
        report["tables"].append(t)
        print("%-18s loop=%08x caller=%s base=%08x end=%08x slots=%d non-null=%d "
              "in_slice=%d bad=%d" % (
                  t["kind"], t["loop"], ("%08x" % t["caller"]) if t.get("caller") else "-",
                  t["base"], t["end"], t["slots"], t["non_null"], t["in_slice"],
                  t["not_function_start"]))
        print("    classes: %s" % t["classes"])
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("wrote %s" % args.json)


def cmd_list(args):
    img = Image(args.exe)
    starts, _ = load_starts(args.functions)
    slset = load_slice(args.slice)
    base, end = parse_range(args.range)
    tbase = int(args.table_base, 16) if args.table_base else base
    rows = table_rows(img, base, end, starts, slset)
    if args.csv:
        print("index,slot,target,region,is_function_start,in_slice,class")
        for r in rows:
            idx = (r["slot"] - tbase) // 4 if tbase <= r["slot"] else r["index"]
            if r["null"]:
                print("%d,0x%08x,,,," % (idx, r["slot"]))
            else:
                print("%d,0x%08x,0x%08x,%s,%s,%s,%s" % (
                    idx, r["slot"], r["target"], r["region"],
                    r["is_function_start"], r["in_slice"], r["class"]))
        return
    for r in rows:
        idx = (r["slot"] - tbase) // 4 if tbase <= r["slot"] else r["index"]
        if r["null"]:
            if args.show_null:
                print("%5d  slot %08x -> (NULL)" % (idx, r["slot"]))
            continue
        if args.limit and idx >= args.limit:
            break
        print("%5d  slot %08x -> %08x  %-8s fn=%-5s slice=%-5s %s" % (
            idx, r["slot"], r["target"], r["region"],
            str(r["is_function_start"]), str(r["in_slice"]), r["class"]))


def parse_range(spec):
    a, b = spec.split(":")
    return int(a, 16), int(b, 16)


def cmd_trace(args):
    img = Image(args.exe)
    starts, _ = load_starts(args.functions)
    slset = load_slice(args.slice)
    src = int(args.src, 16)
    dst = int(args.dst, 16) if args.dst else None
    max_depth = args.depth
    max_edges = args.max

    import bisect

    def fn_end(addr):
        if starts:
            i = bisect.bisect_right(starts, addr)
            if i < len(starts):
                return min(starts[i], addr + 0x4000)
        return addr + 0x4000

    def successors(addr):
        outs = []
        seen = set()
        for ins in img.disasm_upto(addr, fn_end(addr), 2048):
            if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                tgt = int(ins.op_str, 16)
                if tgt not in seen:
                    seen.add(tgt)
                    outs.append((ins.address, tgt, "call"))
            elif ins.mnemonic == "jmp" and ins.op_str.startswith("0x"):
                tgt = int(ins.op_str, 16)
                if tgt not in seen:
                    seen.add(tgt)
                    outs.append((ins.address, tgt, "jmp"))
        return outs

    # Breadth-first: the entry->__tmainCRTStartup->WinMain path is short, while
    # a depth-first walk would disappear into _cinit's constructor subtree.
    parent = {src: None}
    edge = {}
    order = [src]
    queue = collections.deque([(src, 0)])
    found = src == dst
    edges = []
    while queue and len(edges) < max_edges:
        addr, depth = queue.popleft()
        if depth >= max_depth:
            continue
        for site, tgt, kind in successors(addr):
            edges.append((site, tgt, kind))
            if tgt in parent:
                continue
            parent[tgt] = addr
            edge[tgt] = (site, kind)
            order.append(tgt)
            if dst and tgt == dst:
                found = True
                queue.clear()
                break
            queue.append((tgt, depth + 1))
    # Print the ordered first-level calls of each function on the found path,
    # not the whole BFS frontier.
    path = []
    if found:
        cur = dst
        while cur is not None:
            path.append(cur)
            cur = parent.get(cur)
        path.reverse()
        print("PATH: " + " -> ".join("%08x" % p for p in path))
        for p in path:
            print("-- %08x (%s, slice=%s) direct calls:" % (
                p, img.region(p), str(p in slset) if slset else "-"))
            for site, tgt, kind in successors(p)[:args.per_function]:
                print("   %08x %-4s -> %08x %-7s fn=%-5s slice=%s" % (
                    site, kind, tgt, img.region(tgt),
                    str(tgt in starts) if starts else "-",
                    str(tgt in slset) if slset else "-"))
    else:
        print("PATH: %08x -> %08x not found within depth=%d edges=%d" % (
            src, dst or 0, max_depth, len(edges)))
        for site, tgt, kind in edges[:80]:
            print("%08x  %-4s -> %08x  %-8s fn=%-5s slice=%s" % (
                site, kind, tgt, img.region(tgt),
                str(tgt in starts) if starts else "-",
                str(tgt in slset) if slset else "-"))


def cmd_frontier(args):
    img = Image(args.exe)
    starts, _ = load_starts(args.functions)
    slset = load_slice(args.slice)
    base, end = parse_range(args.table)
    after = int(args.after, 16)
    rows = table_rows(img, base, end, starts, slset)
    nz = [r for r in rows if not r["null"]]
    # --after accepts either a slot VA inside the table or a target function VA.
    if base <= after < end and (after - base) % 4 == 0:
        after_slot = after
    else:
        match = [r for r in nz if r["target"] == after]
        after_slot = match[0]["slot"] if match else base - 4
    tail = [r for r in nz if r["slot"] > after_slot]
    print("table base=%08x end=%08x slots=%d non-null=%d null=%d" % (
        base, end, (end - base) // 4, len(nz), (end - base) // 4 - len(nz)))
    print("after slot %08x (target %08x): remaining_non_null=%d in_slice=%d" % (
        after_slot, img.u32(after_slot) or 0, len(tail),
        sum(1 for r in tail if r["in_slice"])))
    print("non-null in slice overall=%d / %d" % (
        sum(1 for r in nz if r["in_slice"]), len(nz)))
    print("classes all: %s" % dict(collections.Counter(r["class"] for r in nz)))
    print("classes remaining: %s" % dict(collections.Counter(r["class"] for r in tail)))
    # Runs of consecutive non-null slots (what table_roots expands per fault).
    runs = []
    cur = []
    for r in tail:
        if cur and r["slot"] != cur[-1]["slot"] + 4:
            runs.append(cur)
            cur = []
        cur.append(r)
    if cur:
        runs.append(cur)
    print("remaining runs=%d sizes=%s" % (len(runs), dict(collections.Counter(len(x) for x in runs))))
    print("next expected table targets:")
    for r in tail[:args.count]:
        print("  index %5d slot %08x -> %08x  %-7s slice=%s" % (
            r["index"], r["slot"], r["target"], r["class"], r["in_slice"]))
    # Missing direct callees: the next non-table blocker.
    missing = collections.defaultdict(list)
    for r in tail:
        for site, tgt in img.direct_calls(r["target"], r["target"] + 128, limit=16):
            if starts and tgt in starts and tgt not in slset:
                missing[tgt].append((r, site))
    print("distinct direct callees missing from the slice: %d" % len(missing))
    for tgt, uses in sorted(missing.items(), key=lambda kv: -len(kv[1]))[:10]:
        r, site = uses[0]
        print("  %08x calls=%4d first_slot=%08x idx=%d -> %08x" % (
            tgt, len(uses), r["slot"], r["index"], r["target"]))
    first = None
    for r in tail:
        for site, tgt in img.direct_calls(r["target"], r["target"] + 128, limit=16):
            if starts and tgt in starts and tgt not in slset:
                first = (r, tgt)
                break
        if first:
            break
    if first:
        print("earliest slot with a missing callee: slot %08x idx %d -> %08x calls %08x" % (
            first[0]["slot"], first[0]["index"], first[0]["target"], first[1]))
    if args.emit_roots:
        lines = "".join("0x%08x\n" % r["target"] for r in tail)
        Path(args.emit_roots).write_text(lines, encoding="ascii")
        print("wrote %d roots to %s (feed to slice_pe.py --extra-root-file)" % (
            len(tail), args.emit_roots))
    if args.emit_callees:
        callees = sorted(missing)
        lines = "".join("0x%08x\n" % t for t in callees)
        Path(args.emit_callees).parent.mkdir(parents=True, exist_ok=True)
        Path(args.emit_callees).write_text(lines, encoding="ascii")
        print("wrote %d distinct missing callees to %s "
              "(same normalized roots format as --emit-roots)" % (
                  len(callees), args.emit_callees))


def cmd_d3d9(args):
    img = Image(args.exe)
    starts, _ = load_starts(args.functions)
    slset = load_slice(args.slice)
    report = {"imports": {}, "strings": {}, "call_sites": [], "renderer_refs": []}
    for name in ("Direct3DCreate9", "d3d9.dll", "D3DPERF_SetOptions"):
        pat = name.encode("ascii")
        addrs = []
        pos = 0
        while True:
            i = img.img.find(pat, pos)
            if i < 0:
                break
            addrs.append(img.base + i)
            pos = i + 1
        report["strings"][name] = addrs[:8]
        print("string %-20s %s" % (name, [("%08x" % a) for a in addrs[:8]]))
    for iat, full in img.iat.items():
        if "d3d9" in full.lower() or "perf" in full.lower() or "direct3d" in full.lower():
            report["imports"]["%08x" % iat] = full
            print("import  %08x  %s" % (iat, full))
    # Direct3DCreate9 call sites: scan the 48 bytes after each string xref for
    # a call through a register (the GetProcAddress result).
    for saddr in report["strings"].get("Direct3DCreate9", [])[:4]:
        for ref in img.dword_refs(saddr):
            if not img.is_text(ref):
                continue
            print("  xref %s @ %08x" % (saddr and "Direct3DCreate9", ref))
            for ins in img.disasm(ref - 16, 40):
                tag = ""
                if ins.mnemonic == "call" and ins.op_str in ("eax", "edx", "ecx", "esi", "edi"):
                    tag = "  <-- indirect API call"
                    report["call_sites"].append({"site": ins.address, "via": ins.op_str})
                print("    %08x  %-24s %-7s %s%s" % (
                    ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, tag))
    # renderer gate references
    gate = int(args.renderer, 16) if args.renderer else 0
    if gate:
        for site in range_scan_call_sites(img, gate):
            report["renderer_refs"].append({"kind": "call", "site": site})
            print("renderer %08x direct call @ %08x" % (gate, site))
        for ref in img.dword_refs(gate):
            if img.is_text(ref) or img.region(ref) in (".rdata", ".data"):
                report["renderer_refs"].append({"kind": "dword", "site": ref})
                print("renderer %08x dword ref @ %08x (%s)" % (gate, ref, img.region(ref)))


def cmd_slots(args):
    """Heuristically list COM vtable slot calls in bounded function ranges.

    Pattern: `mov reg,[base+imm]` (imm <= 0x100) followed by `call reg`.
    Register liveness is tracked only locally; the output is a lead, not proof.
    """
    img = Image(args.exe)
    for spec in args.range:
        lo, hi = parse_range(spec)
        print("== %08x..%08x" % (lo, hi))
        pending = {}
        for ins in img.disasm_upto(lo, hi, 4096):
            op = ins.op_str
            if ins.mnemonic == "mov" and ", " in op:
                dst, src = op.split(", ", 1)
                if dst.startswith("dword ptr [") or "[" in dst:
                    pending.pop(dst, None)
                    continue
                m = None
                if src.startswith("dword ptr [") and "+ 0x" in src:
                    try:
                        base_reg = src.split("[", 1)[1].split(" ")[0]
                        imm = int(src.rsplit("+ 0x", 1)[1].rstrip("]"), 16)
                        if imm <= 0x100:
                            m = imm // 4
                    except Exception:
                        m = None
                if m is not None:
                    pending[dst] = (m, ins.address)
                else:
                    pending.pop(dst, None)
            elif ins.mnemonic == "call" and op in pending:
                slot, site = pending[op]
                print("  slot %2d (0x%02x)  load @ %08x  call @ %08x  [%s]"
                      % (slot, slot * 4, site, ins.address, op))
            elif ins.mnemonic in ("xor", "lea", "pop", "add", "sub", "and", "or"):
                dst = op.split(",", 1)[0]
                pending.pop(dst, None)


def cmd_run(args):
    run = Path(args.run_dir)
    log = run / "probe_run.log" if run.is_dir() else run
    text = log.read_text(encoding="utf-8", errors="replace")
    out = {"log": str(log), "first_fault": None, "tls_callbacks": None,
           "exit": None, "mem": [], "profile": None, "fault_slots": []}
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("first fault:"):
            out["first_fault"] = s
        elif "TLS callbacks" in s:
            out["tls_callbacks"] = s
        elif "ExitProcess" in s and out["exit"] is None:
            out["exit"] = s
        elif s.startswith("[mem] va ") or s.startswith("[mem] pages:"):
            out["mem"].append(s)
    m = None
    if out["first_fault"]:
        import re
        m = re.search(r"Guest fault at ([0-9a-fA-F]+)", out["first_fault"])
    fault = int(m.group(1), 16) if m else None
    if fault:
        img = Image(args.exe)
        starts, funcs = load_starts(args.functions)
        slset = load_slice(args.slice)
        owner = img.containing_function(fault, starts) if starts else None
        out["fault"] = fault
        out["fault_region"] = img.region(fault)
        out["fault_owner"] = owner
        out["fault_owner_in_slice"] = owner in slset if owner else None
        # Find every table slot that points at the fault.
        for ref in img.dword_refs(fault):
            if img.region(ref) in (".rdata", ".data"):
                out["fault_slots"].append({"slot": ref, "region": img.region(ref)})
        print("run: %s" % log)
        print("first fault: %s" % out["first_fault"])
        print("fault %08x region=%s owner=%s in_slice=%s" % (
            fault, out["fault_region"],
            ("%08x" % owner) if owner else "-", out["fault_owner_in_slice"]))
        for s in out["fault_slots"]:
            print("  pointer slot %08x (%s)" % (s["slot"], s["region"]))
    if out["tls_callbacks"]:
        print("tls: %s" % out["tls_callbacks"])
    if out["exit"]:
        print("exit: %s" % out["exit"])
    for s in out["mem"]:
        print(s)
    profile = run / "tesv_profile.csv" if run.is_dir() else None
    if profile and profile.exists():
        rows = []
        for i, line in enumerate(profile.read_text(encoding="utf-8").splitlines()):
            if i == 0 or not line:
                continue
            a, h = line.split(",")[:2]
            rows.append((int(a, 16), int(h)))
        rows.sort(key=lambda x: -x[1])
        out["profile"] = {"distinct": len(rows), "top": rows[:args.top]}
        print("profile: %d distinct, top %d" % (len(rows), min(args.top, len(rows))))
        for a, h in rows[:args.top]:
            print("  %08x hits=%d in_slice=%s" % (a, h, str(a in slset)))
    if args.json:
        Path(args.json).write_text(json.dumps(out, indent=2), encoding="utf-8")
        print("wrote %s" % args.json)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--exe", default=r"E:\skyr\TESV.exe")
    common.add_argument("--functions", default=r"E:\Skyrim_N3DS_build\analysis\tesv\functions.json")
    common.add_argument("--slice", default=None)

    ap = argparse.ArgumentParser(description=__doc__,
                                 parents=[common],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("pe", parents=[common])

    p = sub.add_parser("tables", parents=[common])
    p.add_argument("--json", default=None)

    p = sub.add_parser("list", parents=[common])
    p.add_argument("--range", required=True, help="base:end (hex VAs)")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--show-null", action="store_true")
    p.add_argument("--table-base", default=None,
                   help="table base for absolute index numbers (default: range base)")
    p.add_argument("--csv", action="store_true")

    p = sub.add_parser("trace", parents=[common])
    p.add_argument("--src", default="0xf568a1")
    p.add_argument("--dst", default="0x69d1d0")
    p.add_argument("--depth", type=int, default=6)
    p.add_argument("--max", type=int, default=400)
    p.add_argument("--per-function", type=int, default=16)

    p = sub.add_parser("frontier", parents=[common])
    p.add_argument("--table", required=True, help="base:end (hex VAs)")
    p.add_argument("--after", required=True, help="slot or target address (hex)")
    p.add_argument("--count", type=int, default=16)
    p.add_argument("--emit-roots", default=None)
    p.add_argument("--emit-callees", default=None,
                   help="write every distinct missing direct callee (roots format)")

    p = sub.add_parser("d3d9", parents=[common])
    p.add_argument("--renderer", default="0xc6dbf0")

    p = sub.add_parser("slots", parents=[common])
    p.add_argument("--range", action="append", required=True,
                   help="function range base:end; repeatable")

    p = sub.add_parser("run", parents=[common])
    p.add_argument("run_dir")
    p.add_argument("--top", type=int, default=12)
    p.add_argument("--json", default=None)

    args = ap.parse_args()
    if args.cmd == "pe":
        cmd_pe(args)
    elif args.cmd == "tables":
        cmd_tables(args)
    elif args.cmd == "list":
        cmd_list(args)
    elif args.cmd == "trace":
        cmd_trace(args)
    elif args.cmd == "frontier":
        cmd_frontier(args)
    elif args.cmd == "d3d9":
        cmd_d3d9(args)
    elif args.cmd == "slots":
        cmd_slots(args)
    elif args.cmd == "run":
        cmd_run(args)


if __name__ == "__main__":
    main()
