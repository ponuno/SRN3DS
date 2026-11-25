"""Bounded call-graph slice over an upstream discover.py functions.json.

Direct calls are followed; calls/jumps through the IAT are recorded as imports;
other indirect transfers are recorded for diagnostics. Optionally emits a
functions.json subset that staged/lift.py can consume.

Functions are admitted to the slice at enqueue time, so the function budget can
never silently leave a direct callee out: after the BFS the slice is pruned to a
closed set. A slice that would have to drop one of its roots is refused with an
explicit budget error instead of being emitted broken.

Call/jump targets that land in an executable PE section but are missing from
the discovery data are decoded locally with capstone (bounded recursive
descent), so the slice does not fault one address at a time at runtime.

Data-slot roots (absolute memory operands, e.g. `push [0x12de4a0]`) and data
table pointers referenced by immediates are read from .data/.rdata and admitted
when they hold plausible function starts. PE TLS callbacks are added as roots
as well.

With --allow-open the admitted set is emitted without closure pruning. The
report then says "mode": "open", marks the slice as not closed, and lists the
cut direct targets that remain for the fault-driven growth loop.

Usage:
  python tools/slice_pe.py EXE FUNCTIONS_JSON [--root HEX|entry] [--max-functions N]
      [--max-insns M] [--extra-root HEX]... [--extra-root-file PATH]...
      [--local-budget N] [--out DIR] [--emit] [--allow-open]
"""
import argparse
import json
import sys
from collections import Counter, defaultdict, deque
from pathlib import Path

import capstone
import pefile
from capstone import x86


IMAGE_SCN_MEM_EXECUTE = 0x20000000
PAD_BYTES = (0x90, 0xCC)
IAT_THUNK_PREFIX = b"\xff\x25"
MAX_DATA_RUN = 8
MAX_DATA_ROOTS_PER_FUNCTION = 4
MAX_DATA_ROOTS_TOTAL = 256
TLS_MAX_CALLBACKS = 16
TLS_MAX_LEADING_NULLS = 2
IMAGE_DIRECTORY_ENTRY_TLS = 9


def merge_ranges(ranges):
    """Sort and merge overlapping [lo, hi) ranges."""
    ranges = sorted(ranges)
    merged = []
    for lo, hi in ranges:
        if merged and lo <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
        else:
            merged.append((lo, hi))
    return merged


def load_image(path, base):
    pe = pefile.PE(path)
    image = bytes(pe.get_memory_mapped_image(ImageBase=base))
    return pe, image


def executable_ranges(pe, base, image_len):
    """Absolute VA ranges of sections mapped with IMAGE_SCN_MEM_EXECUTE."""
    ranges = []
    for section in pe.sections:
        if not section.Characteristics & IMAGE_SCN_MEM_EXECUTE:
            continue
        size = section.Misc_VirtualSize or section.SizeOfRawData
        if size <= 0:
            continue
        lo = base + section.VirtualAddress
        hi = min(lo + size, base + image_len)
        if hi > lo:
            ranges.append((lo, hi))
    return merge_ranges(ranges)


def data_section_ranges(pe, base, image_len, names=(b".data", b".rdata")):
    """Absolute VA ranges of the non-executable .data/.rdata sections."""
    wanted = {name.lower() for name in names}
    ranges = []
    for section in pe.sections:
        name = section.Name.rstrip(b"\x00").lower()
        if name not in wanted:
            continue
        if section.Characteristics & IMAGE_SCN_MEM_EXECUTE:
            continue
        size = section.Misc_VirtualSize or section.SizeOfRawData
        if size <= 0:
            continue
        lo = base + section.VirtualAddress
        hi = min(lo + size, base + image_len)
        if hi > lo:
            ranges.append((lo, hi))
    return merge_ranges(ranges)


def parse_addr(text):
    """Parse a hex address: 0x1234, 1234h or plain 1234."""
    token = text.strip()
    if token.lower().startswith("0x"):
        token = token[2:]
    if token[-1:] in ("h", "H"):
        token = token[:-1]
    return int(token, 16)


def read_root_file(path):
    """Read one hex address per line; blank lines and # comments are ignored."""
    roots = []
    for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        roots.append(parse_addr(line.split()[0].rstrip(",")))
    return roots


def range_limit(addr, ranges):
    """End of the range containing addr, or None."""
    for lo, hi in ranges:
        if lo <= addr < hi:
            return hi
    return None


def in_range(addr, ranges):
    return range_limit(addr, ranges) is not None


def is_iat_thunk(addr, iat, image, base):
    """True for an IAT slot address or an `jmp dword ptr [iat_slot]` thunk."""
    if addr in iat:
        return True
    off = addr - base
    if 0 <= off <= len(image) - 6 and image[off:off + 2] == IAT_THUNK_PREFIX:
        slot = int.from_bytes(image[off + 2:off + 6], "little")
        return slot in iat
    return False


def looks_like_function(image, base, addr):
    """Cheap prologue test for a candidate function start.

    Besides the classic MSVC frame prologues this accepts the "this-call thunk"
    and "atexit handler" shapes emitted for global constructors/destructors:
    `B9 imm32 E9 rel32` (mov ecx,obj; jmp fn), `B9 imm32 E8 rel32`,
    `B9 imm32 C3` (mov ecx,obj; ret) and a bare `E9 rel32` jump thunk.
    Without them, registered destructors are treated as data and their bodies
    are never lifted, which shows up as one budget-cut fault per handler.
    """
    off = addr - base
    if off < 0 or off + 2 > len(image):
        return False
    head = image[off:off + 2]
    if head in (b"\x8b\xff", b"\x55\x8b", b"\x83\xec"):
        return True
    if head[:1] == b"\xb9" and off + 6 <= len(image):
        tail = image[off + 5:off + 6]
        if tail in (b"\xe9", b"\xe8", b"\xc3"):
            return True
    return head[:1] == b"\xe9"


def printable_ascii_u32(value):
    """True when the low 32 bits are four printable ASCII bytes."""
    return all(0x20 <= byte <= 0x7E for byte in value.to_bytes(4, "little"))


def function_run_at(off, image, base, exec_ranges, funcs, max_run=MAX_DATA_RUN,
                    max_leading_nulls=0, require_shape=True):
    """Function starts in consecutive dwords at an image offset.

    Data tables referenced by an immediate may legitimately contain short
    virtual methods (`mov eax,ecx; ret`, bare `ret`, etc.). Their membership in
    the discovery manifest is the strong signal there, so data_table_run()
    disables the prologue-shape heuristic while data slots keep it.
    """
    roots = []
    leading_nulls = 0
    index = 0
    limit = max_leading_nulls + max_run + 2
    while index < limit and len(roots) < max_run:
        pos = off + 4 * index
        if pos < 0 or pos + 4 > len(image):
            break
        value = int.from_bytes(image[pos:pos + 4], "little")
        if value == 0:
            if not roots and leading_nulls < max_leading_nulls:
                leading_nulls += 1
                index += 1
                continue
            break
        if value == 0xFFFFFFFF:
            break
        if not in_range(value, exec_ranges):
            break
        if value not in funcs:
            break
        if printable_ascii_u32(value):
            break
        if require_shape and not looks_like_function(image, base, value):
            break
        roots.append(value)
        index += 1
    return roots


def data_slot_run(slot, image, base, exec_ranges, funcs, data_ranges, iat,
                  max_run=MAX_DATA_RUN):
    """Function starts in the dword run at an absolute data slot.

    The slot must live in .data/.rdata and must not be an IAT slot. The run
    stops at the first dword that is zero, all-ones, outside executable code,
    missing from the discovery functions, printable ASCII, or not shaped like
    a function prologue.
    """
    if slot in iat or not in_range(slot, data_ranges):
        return []
    return function_run_at(slot - base, image, base, exec_ranges, funcs, max_run)


def data_table_run(addr, image, base, exec_ranges, funcs, data_ranges, iat,
                   max_run=MAX_DATA_RUN):
    """Function-pointer table at a data address, tolerating leading NULLs.

    This catches callback tables referenced by an immediate operand (e.g.
    `push 0x1076a9c` or `mov dword ptr [esp], 0x1076a98`) as well as data
    slots that hold a pointer to such a table.
    """
    if addr in iat or not in_range(addr, data_ranges):
        return []
    return function_run_at(addr - base, image, base, exec_ranges, funcs,
                           max_run, max_leading_nulls=TLS_MAX_LEADING_NULLS,
                           require_shape=False)


def tls_callback_roots(pe, base, image, max_callbacks=TLS_MAX_CALLBACKS,
                       max_leading_nulls=TLS_MAX_LEADING_NULLS):
    """PE TLS callback addresses (AddressOfCallBacks array).

    Up to two leading NULLs are tolerated; the array ends at the next NULL.
    """
    try:
        directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[IMAGE_DIRECTORY_ENTRY_TLS]
    except (AttributeError, IndexError, TypeError):
        return []
    rva = getattr(directory, "VirtualAddress", 0) or 0
    if not rva or rva + 16 > len(image):
        return []
    callbacks = int.from_bytes(image[rva + 12:rva + 16], "little")
    if callbacks in (0, 0xFFFFFFFF):
        return []
    callbacks_off = callbacks - base
    if callbacks_off < 0 or callbacks_off + 4 > len(image):
        return []

    roots = []
    leading_nulls = 0
    index = 0
    limit = max_leading_nulls + max_callbacks + 2
    while index < limit and len(roots) < max_callbacks:
        pos = callbacks_off + 4 * index
        if pos + 4 > len(image):
            break
        value = int.from_bytes(image[pos:pos + 4], "little")
        if value == 0:
            if not roots and leading_nulls < max_leading_nulls:
                leading_nulls += 1
                index += 1
                continue
            break
        if value in (0, 0xFFFFFFFF):
            break
        roots.append(value)
        index += 1
    return roots


class Admission:
    """Budgeted admission of functions into the slice at enqueue time."""

    def __init__(self, funcs, iat, exec_ranges, image, base, max_functions):
        self.funcs = funcs
        self.iat = iat
        self.exec_ranges = exec_ranges
        self.image = image
        self.base = base
        self.max_functions = max_functions
        self.admitted = set()
        self.queue = deque()
        self.cut = {}
        self.rejected = Counter()

    def admit(self, addr):
        """Admit addr once; return True when it is (already) in the slice."""
        if addr in self.admitted:
            return True
        if not in_range(addr, self.exec_ranges):
            self.rejected["not executable"] += 1
            return False
        if is_iat_thunk(addr, self.iat, self.image, self.base):
            self.rejected["iat thunk"] += 1
            return False
        if addr not in self.funcs:
            self.rejected["not in discovery"] += 1
            return False
        if len(self.admitted) >= self.max_functions:
            self.cut[addr] = "function budget"
            return False
        self.admitted.add(addr)
        self.queue.append(addr)
        return True


def prune_to_closure(selected, edges, valid):
    """Drop functions with direct edges to valid functions outside the slice.

    Repeats to a fixed point via reverse edges, so the returned set is closed
    under the supplied direct edges. Returns (kept, dropped).
    """
    kept = set(selected)
    dropped = set()
    reverse = defaultdict(set)
    queue = deque()
    queued = set()

    for fn in selected:
        for target in edges.get(fn, ()):
            if target == fn or not valid(target):
                continue
            reverse[target].add(fn)
            if target not in kept and fn not in queued:
                queue.append(fn)
                queued.add(fn)

    while queue:
        fn = queue.popleft()
        if fn not in kept:
            continue
        kept.remove(fn)
        dropped.add(fn)
        for caller in reverse.get(fn, ()):
            if caller in kept and caller not in queued:
                queue.append(caller)
                queued.add(caller)
    return kept, dropped


def budget_error_message(admitted, cut, max_functions):
    """Explicit failure text when the function budget cannot close the slice."""
    needed = len(admitted) + len(cut)
    return (f"budget too small for a closed slice; need >= {needed} functions "
            f"(max-functions {max_functions}, admitted {len(admitted)}, "
            f"{len(cut)} direct targets cut)")


def split_block_at(blocks, addr, insn_starts):
    """Split the block containing addr so addr becomes a block start."""
    if addr not in insn_starts:
        return False
    for index, (lo, hi) in enumerate(blocks):
        if lo < addr < hi:
            blocks[index] = (lo, addr)
            blocks.insert(index + 1, (addr, hi))
            return True
    return False


def is_padding(ins, image, base, limit):
    """True for int3 or a run of 0x90/0xCC padding bytes."""
    if ins.mnemonic == "int3":
        return True
    if ins.mnemonic == "nop" and ins.bytes and all(b == 0x90 for b in ins.bytes):
        nxt = ins.address + ins.size
        if nxt < limit:
            off = nxt - base
            if 0 <= off < len(image) and image[off] in PAD_BYTES:
                return True
    return False


def jump_table_targets(md, ins, image, base, exec_ranges, start, limit=256,
                       span=0x20000):
    """Targets of `jmp [index*4 + table]` when the table holds code pointers."""
    if not ins.operands:
        return None
    op = ins.operands[0]
    if op.type != x86.X86_OP_MEM or op.mem.base or not op.mem.index or op.mem.scale != 4:
        return None
    table = op.mem.disp & 0xFFFFFFFF
    if range_limit(table, exec_ranges) is not None:
        return None  # a table lives in data, not in code
    off = table - base
    if off < 0 or off + 4 > len(image):
        return None
    targets = []
    for i in range(min(limit, (len(image) - off) // 4)):
        target = int.from_bytes(image[off + 4 * i:off + 4 * i + 4], "little")
        if range_limit(target, exec_ranges) is None:
            break
        targets.append(target)
    # Reject tables that spill out of the function: those tend to be global
    # callback/function-pointer tables, not switch tables.
    if len(targets) < 2 or any(not start <= t <= start + span for t in targets):
        return None
    return targets


def build_function_blocks(md, image, base, exec_ranges, start, budget=4096,
                          jump_table_limit=256, jump_table_span=0x20000):
    """Recursive-descent decode of one function.

    Follows direct jumps and conditional-jump targets, keeps call fallthrough
    (calls are not followed), stops at returns, indirect transfers, padding,
    already covered code and the instruction budget. Returns
    (blocks, jump_tables, stats) with blocks as [lo, hi] absolute ranges,
    compatible with staged/lift.py's `functions` format.
    """
    blocks = []
    tables = {}
    covered = set()
    insn_starts = set()
    pending = deque([start])
    seen = set()
    count = 0
    truncated = False
    failed = []

    def enqueue(addr):
        if addr is None:
            return
        if addr in seen:
            return
        if range_limit(addr, exec_ranges) is None:
            return
        pending.append(addr)

    while pending:
        block_start = pending.popleft()
        if block_start in seen:
            continue
        seen.add(block_start)
        if range_limit(block_start, exec_ranges) is None:
            continue
        if block_start in covered:
            split_block_at(blocks, block_start, insn_starts)
            continue

        lo = block_start
        cursor = block_start
        while True:
            limit = range_limit(cursor, exec_ranges)
            if limit is None or cursor in covered:
                break
            if count >= budget:
                truncated = True
                break
            off = cursor - base
            size = min(15, limit - cursor, len(image) - off)
            if size <= 0:
                break
            ins = next(md.disasm(image[off:off + size], cursor), None)
            if ins is None:
                failed.append(cursor)
                break
            if is_padding(ins, image, base, limit):
                break

            covered.add(cursor)
            insn_starts.add(cursor)
            count += 1
            cursor = ins.address + ins.size
            mn = ins.mnemonic
            groups = set(ins.groups)
            op = ins.operands[0] if ins.operands else None

            if mn == "call":
                enqueue(cursor)
                break
            if mn in ("ret", "retf", "iret", "iretd") or x86.X86_GRP_RET in groups:
                break
            if mn == "jmp":
                if op is not None and op.type == x86.X86_OP_IMM:
                    enqueue(op.imm & 0xFFFFFFFF)
                elif op is not None and op.type == x86.X86_OP_MEM:
                    targets = jump_table_targets(md, ins, image, base, exec_ranges,
                                                 start, jump_table_limit,
                                                 jump_table_span)
                    if targets:
                        tables[ins.address] = targets
                        for target in targets:
                            enqueue(target)
                break
            if mn in ("ljmp", "lcall"):
                break
            if mn in ("loop", "loope", "loopne", "jecxz", "jrcxz") or \
                    x86.X86_GRP_JUMP in groups:
                if op is not None and op.type == x86.X86_OP_IMM:
                    enqueue(op.imm & 0xFFFFFFFF)
                enqueue(cursor)
                break

        if cursor > lo:
            blocks.append((lo, cursor))

    blocks.sort()
    stats = {"instructions": count, "truncated": truncated,
             "failed": failed, "blocks": len(blocks)}
    return blocks, tables, stats


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("exe", type=Path)
    ap.add_argument("functions", type=Path)
    ap.add_argument("--root", default="entry")
    ap.add_argument("--extra-root", action="append", default=[],
                    help="additional function start (hex) to include, e.g. a TLS callback")
    ap.add_argument("--extra-root-file", action="append", type=Path, default=[],
                    help="file with one hex function start per line (# comments allowed)")
    ap.add_argument("--max-functions", type=int, default=40000)
    ap.add_argument("--max-insns", type=int, default=4000000)
    ap.add_argument("--local-budget", type=int, default=4096,
                    help="instruction budget per locally decoded function")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--allow-open", action="store_true",
                    help="emit the admitted set without pruning to a closed slice")
    ap.add_argument("--no-entry-root", action="store_true",
                    help="traverse only from --extra-root/TLS roots (executed-set measurement)")
    args = ap.parse_args()

    data = json.loads(args.functions.read_text(encoding="utf-8"))
    base = data["image_base"]
    pe, image = load_image(args.exe, base)
    exec_ranges = executable_ranges(pe, base, len(image))
    data_ranges = data_section_ranges(pe, base, len(image))
    if not exec_ranges:
        print("warning: no executable PE sections found")
    entry = data["entry"] if args.root == "entry" else parse_addr(args.root)
    funcs = {int(k, 16): [tuple(b) for b in v] for k, v in data["functions"].items()}
    discovered_count = len(funcs)
    iat = {int(k, 16): v for k, v in data["iat"].items()}
    jump_tables = {int(k, 16): [int(t, 16) for t in v] for k, v in data["jump_tables"].items()}
    indirect_sites = {int(a, 16) for a in data["indirect_jumps"]}

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    def u32(addr):
        off = addr - base
        return int.from_bytes(image[off:off + 4], "little") if 0 <= off <= len(image) - 4 else None

    def import_via(target):
        if target in iat:
            return iat[target]
        off = target - base
        if 0 <= off <= len(image) - 6 and image[off] == 0xFF and image[off + 1] == 0x25:
            slot = int.from_bytes(image[off + 2:off + 6], "little")
            return iat.get(slot)
        return None

    def in_exec(addr):
        return range_limit(addr, exec_ranges) is not None

    extra_roots = [parse_addr(x) for x in args.extra_root]
    for path in args.extra_root_file:
        extra_roots.extend(read_root_file(path))
    tls_roots = tls_callback_roots(pe, base, image)
    if args.no_entry_root:
        # The executed-set measurement: traverse only from the supplied roots.
        known_roots = set(extra_roots)
        roots = list(extra_roots) + [a for a in tls_roots if a not in known_roots]
        tls_roots_added = [a for a in tls_roots if a not in known_roots]
    else:
        known_roots = {entry, *extra_roots}
        tls_roots_added = [addr for addr in tls_roots if addr not in known_roots]
        roots = [entry] + extra_roots + tls_roots_added

    data_roots = set()
    data_root_candidates = set()
    data_root_cut = set()
    push_imm_roots = set()
    total_insns = 0
    x86_bytes = 0
    direct_edges = 0
    external_calls = Counter()
    external_detail = {}
    local_decoded = {}
    local_failures = {}
    local_jump_tables = set()
    iat_calls = Counter()
    dynamic_sites = []
    jump_table_hits = 0
    used_jump_tables = set()
    unsupported = Counter()
    stopped = None
    edges = defaultdict(set)
    scanned = set()

    def try_local_decode(target):
        """Decode a function missing from discovery; insert it into funcs."""
        if target in funcs:
            return funcs[target]
        if target in local_failures:
            return None
        if not in_exec(target):
            local_failures[target] = "target is not in an executable section"
            return None
        blocks, tables, stats = build_function_blocks(
            md, image, base, exec_ranges, target, budget=args.local_budget)
        if not blocks:
            where = hex(stats["failed"][0]) if stats["failed"] else "no decodable instructions"
            local_failures[target] = "local decode failed at " + where
            return None
        funcs[target] = blocks
        for site, targets in tables.items():
            if site not in jump_tables:
                jump_tables[site] = targets
                local_jump_tables.add(site)
        local_decoded[target] = {"blocks": len(blocks),
                                 "instructions": stats["instructions"],
                                 "truncated": stats["truncated"]}
        return blocks

    def record_external(target):
        external_calls[target] += 1
        if target not in external_detail:
            external_detail[target] = {
                "in_exec": in_exec(target),
                "local_decode": local_failures.get(target, "not attempted"),
            }

    def resolve_function(target):
        """Return target when it is a callable function, local-decoding if needed."""
        if is_iat_thunk(target, iat, image, base):
            return None
        if not in_exec(target):
            return None
        if target in funcs:
            return target
        if try_local_decode(target) is not None:
            return target
        return None

    admission = Admission(funcs, iat, exec_ranges, image, base, args.max_functions)

    roots_admitted = []
    for root in roots:
        if root not in funcs:
            try_local_decode(root)
        if admission.admit(root):
            roots_admitted.append(root)
        else:
            record_external(root)

    while admission.queue:
        fn = admission.queue.popleft()
        if fn in scanned:
            continue
        scanned.add(fn)
        blocks = funcs.get(fn)
        if blocks is None:
            continue
        fn_data_roots = set()
        seen_slots = set()
        for lo, hi in blocks:
            x86_bytes += hi - lo
            off = lo - base
            if off < 0 or off + (hi - lo) > len(image):
                unsupported["block outside image"] += 1
                continue
            for ins in md.disasm(image[off:off + (hi - lo)], lo):
                total_insns += 1
                if total_insns >= args.max_insns:
                    stopped = f"instruction budget {args.max_insns}"
                    break
                mn = ins.mnemonic
                if mn == "push":
                    for dop in ins.operands:
                        if dop.type != x86.X86_OP_IMM:
                            continue
                        target = dop.imm & 0xffffffff
                        if target != fn and target in funcs and \
                                looks_like_function(image, base, target):
                            edges[fn].add(target)
                            push_imm_roots.add(target)
                            admission.admit(target)
                if mn in {"int3", "hlt", "ud2", "retf", "iretd", "syscall", "sysenter"}:
                    unsupported[mn] += 1
                if ins.id == x86.X86_INS_CALL:
                    op = ins.operands[0] if ins.operands else None
                    if op is None:
                        unsupported["call without operand"] += 1
                    elif op.type == x86.X86_OP_IMM:
                        target = op.imm & 0xffffffff
                        name = import_via(target)
                        if name:
                            iat_calls[name] += 1
                        elif resolve_function(target) is not None:
                            direct_edges += 1
                            edges[fn].add(target)
                            admission.admit(target)
                        else:
                            record_external(target)
                    elif op.type == x86.X86_OP_MEM and not op.mem.base and not op.mem.index:
                        slot = op.mem.disp & 0xffffffff
                        name = iat.get(slot)
                        if name:
                            iat_calls[name] += 1
                        else:
                            target = u32(slot)
                            if target is not None and resolve_function(target) is not None:
                                direct_edges += 1
                                edges[fn].add(target)
                                admission.admit(target)
                            else:
                                dynamic_sites.append((ins.address, "call [%08x]" % slot))
                    else:
                        dynamic_sites.append((ins.address, "call indirect"))
                elif ins.id == x86.X86_INS_JMP:
                    op = ins.operands[0] if ins.operands else None
                    if op is None:
                        continue
                    if op.type == x86.X86_OP_IMM:
                        target = op.imm & 0xffffffff
                        if target == fn:
                            continue
                        if resolve_function(target) is not None:
                            direct_edges += 1
                            edges[fn].add(target)
                            admission.admit(target)
                        else:
                            record_external(target)
                    elif op.type == x86.X86_OP_MEM and not op.mem.base and not op.mem.index:
                        slot = op.mem.disp & 0xffffffff
                        name = iat.get(slot)
                        if name:
                            iat_calls[name] += 1
                        else:
                            target = u32(slot)
                            if target is not None and resolve_function(target) is not None:
                                direct_edges += 1
                                edges[fn].add(target)
                                admission.admit(target)
                            else:
                                dynamic_sites.append((ins.address, "jmp [%08x]" % slot))
                    elif ins.address in jump_tables:
                        jump_table_hits += 1
                        used_jump_tables.add(ins.address)
                    else:
                        dynamic_sites.append((ins.address, "jmp indirect"))
                else:
                    # Absolute data-slot roots: `push [slot]`, `mov eax, [slot]`, ...
                    # Data addresses in immediates may also point at callback
                    # tables (`push 0x1076a9c`, `mov [esp], 0x1076a98`).
                    data_refs = []
                    for dop in ins.operands:
                        if dop.type == x86.X86_OP_MEM and not dop.mem.base and \
                                not dop.mem.index:
                            data_refs.append((dop.mem.disp & 0xffffffff, data_slot_run))
                        elif dop.type == x86.X86_OP_IMM:
                            value = dop.imm & 0xffffffff
                            if in_range(value, data_ranges):
                                data_refs.append((value, data_table_run))
                    for addr, reader in data_refs:
                        if addr in seen_slots:
                            continue
                        seen_slots.add(addr)
                        if addr in iat:
                            continue
                        run = reader(addr, image, base, exec_ranges, funcs,
                                     data_ranges, iat)
                        for index, target in enumerate(run):
                            if len(fn_data_roots) >= MAX_DATA_ROOTS_PER_FUNCTION or \
                                    len(data_root_candidates) >= MAX_DATA_ROOTS_TOTAL:
                                data_root_cut.update(run[index:])
                                break
                            fn_data_roots.add(target)
                            data_root_candidates.add(target)
                            if target not in admission.admitted:
                                if admission.admit(target):
                                    data_roots.add(target)
                                else:
                                    data_root_cut.add(target)
            if stopped:
                break
        if stopped:
            break

    def project(insns):
        return int(insns * 56.9)

    def valid_target(target):
        return target in funcs and in_exec(target)

    if args.allow_open:
        kept = set(admission.admitted)
        pruned = set()
        closed = stopped is None and not admission.cut
    else:
        kept, pruned = prune_to_closure(admission.admitted, edges, valid_target)
        closed = stopped is None
        budget_roots = [root for root in roots if root in admission.cut]
        dropped_roots = [root for root in roots_admitted if root not in kept]
        if budget_roots or dropped_roots:
            print("error:", budget_error_message(admission.admitted, admission.cut,
                                                 args.max_functions))
            if budget_roots:
                print("  roots cut by the budget:",
                      ", ".join(hex(a) for a in budget_roots[:8]))
            if dropped_roots:
                print("  roots dropped by pruning:",
                      ", ".join(hex(a) for a in dropped_roots[:8]))
            print("  cut direct targets:",
                  ", ".join(hex(a) for a in sorted(admission.cut)[:8]))
            return 2
    if not args.no_entry_root and entry not in admission.admitted:
        if entry in admission.cut:
            print("error:", budget_error_message(admission.admitted, admission.cut,
                                                 args.max_functions))
        else:
            print(f"error: root {entry:#x} was not admitted as a discovered executable function")
        return 2

    external_entries = []
    for addr, count in external_calls.most_common():
        detail = external_detail.get(addr, {})
        external_entries.append({
            "addr": hex(addr),
            "count": count,
            "in_exec": detail.get("in_exec", in_exec(addr)),
            "local_decode": detail.get("local_decode", "not attempted"),
        })

    report = {
        "mode": "open" if args.allow_open else "closed",
        "root": hex(entry),
        "root_admitted": entry in admission.admitted,
        "roots": [hex(a) for a in roots],
        "roots_admitted": [hex(a) for a in roots_admitted],
        "extra_roots": [hex(a) for a in extra_roots],
        "tls_roots": [hex(a) for a in tls_roots],
        "tls_roots_added": [hex(a) for a in tls_roots_added],
        "selected_functions": len(kept),
        "admitted_functions": len(admission.admitted),
        "pruned_functions": len(pruned),
        "pruned_addresses": [hex(a) for a in sorted(pruned)],
        "closed": closed,
        "cut_target_count": len(admission.cut),
        "cut_targets": [hex(a) for a in sorted(admission.cut)],
        "total_discovered_functions": discovered_count,
        "functions_after_local_decode": len(funcs),
        "local_decoded": len(local_decoded),
        "local_decoded_addresses": [hex(a) for a in sorted(local_decoded)],
        "local_decoded_detail": {hex(a): v for a, v in sorted(local_decoded.items())},
        "executable_ranges": [{"lo": hex(lo), "hi": hex(hi)} for lo, hi in exec_ranges],
        "data_ranges": [{"lo": hex(lo), "hi": hex(hi)} for lo, hi in data_ranges],
        "x86_bytes": x86_bytes,
        "x86_instructions": total_insns,
        "direct_call_edges": direct_edges,
        "jump_table_sites": jump_table_hits,
        "data_roots": len(data_roots),
        "data_root_candidates": len(data_root_candidates),
        "data_root_addresses": [hex(a) for a in sorted(data_roots)],
        "data_root_cut": len(data_root_cut),
        "data_root_cut_addresses": [hex(a) for a in sorted(data_root_cut)],
        "push_imm_roots": len(push_imm_roots),
        "projected_arm_text_bytes": project(total_insns),
        "projected_arm_text_mib": round(project(total_insns) / 1048576, 1),
        "stopped": stopped,
        "imports": [{"name": n, "call_sites": c} for n, c in iat_calls.most_common()],
        "external_calls_total": sum(external_calls.values()),
        "external_calls": external_entries[:60],
        "dynamic_sites_total": len(dynamic_sites),
        "dynamic_sites_sample": [{"addr": hex(a), "kind": k} for a, k in dynamic_sites[:60]],
        "unsupported": dict(unsupported),
    }

    print(f"root {entry:#x}: {len(kept)}/{len(funcs)} functions, "
          f"{x86_bytes} x86 bytes, {total_insns} instructions, "
          f"{len(iat_calls)} distinct imports, {len(dynamic_sites)} dynamic sites")
    print(f"admitted {len(admission.admitted)}, pruned {len(pruned)}, "
          f"cut {len(admission.cut)}, closed {closed}")
    if args.allow_open:
        print(f"open slice: {len(admission.admitted)} admitted, "
              f"{len(admission.cut)} cut targets "
              f"(first executed cut target will fault)")
    print(f"projected ARM .text ~{report['projected_arm_text_mib']} MiB (56.9 B/insn)")
    if tls_roots:
        print(f"TLS callbacks: {len(tls_roots)} ({len(tls_roots_added)} added):",
              ", ".join(hex(a) for a in tls_roots[:8]))
    if local_decoded:
        print(f"locally decoded {len(local_decoded)} functions:",
              ", ".join(hex(a) for a in sorted(local_decoded)[:8]))
    if data_roots or data_root_cut:
        print(f"data-slot roots: {len(data_roots)} added, {len(data_root_cut)} cut, "
              f"{len(data_root_candidates)} candidates")
    if stopped:
        print("STOPPED:", stopped)
    print("top imports:", ", ".join(f"{n} x{c}" for n, c in iat_calls.most_common(12)))
    if external_calls:
        in_exec_count = sum(1 for a in external_calls if in_exec(a))
        print(f"unclassified direct targets: {len(external_calls)} "
              f"({in_exec_count} in executable sections), e.g.",
              ", ".join(hex(a) for a, _ in external_calls.most_common(5)))

    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "slice_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("report:", args.out / "slice_report.json")

    if args.emit:
        out_dir = args.out or args.functions.parent
        emitted = {"image_base": base, "entry": entry,
                   "functions": {hex(a): [list(b) for b in funcs[a]] for a in sorted(kept)},
                   "iat": {hex(k): v for k, v in iat.items()},
                   "jump_tables": {hex(k): [hex(t) for t in v] for k, v in jump_tables.items()
                                   if k in used_jump_tables or k in local_jump_tables},
                   "indirect_jumps": [hex(a) for a in sorted(indirect_sites)]}
        path = out_dir / "slice_functions.json"
        path.write_text(json.dumps(emitted), encoding="utf-8")
        print("slice:", path)

    return 0


if __name__ == "__main__":
    sys.exit(main())
