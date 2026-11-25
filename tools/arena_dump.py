"""lldb helper: dump the TESV arena state at the guest fault.

Loaded with:
  lldb --batch -o "breakpoint set -n rt_unimpl" \
       -o "run E:\\skyr\\TESV.exe . 200000 169000000" \
       -o "command script import tools/arena_dump.py" \
       -o "arena_dump" ...

The guest's GuestMemory lives in BootSession b inside run() (tesv_slice_main.c),
so every guest read is translated through that region table: the host build
allocates guest memory with calloc(), it is not mapped at the guest VA.
"""
import json
import struct

import lldb

ARENA_BASE = 0x40000000
POOL_OBJ = 0x1B418B0


def out(s=""):
    print(s)


class Guest:
    def __init__(self, frame):
        b = frame.FindVariable("b")
        if not b or not b.IsValid():
            raise RuntimeError("no BootSession b in run()")
        self.mem = b.GetChildMemberWithName("memory")
        if not self.mem or not self.mem.IsValid():
            raise RuntimeError("no b.memory")
        self.regions = []
        arr = self.mem.GetChildMemberWithName("regions")
        count = self.mem.GetChildMemberWithName("count").GetValueAsUnsigned()
        self.count = count
        for i in range(count):
            r = arr.GetChildAtIndex(i)
            base = r.GetChildMemberWithName("base").GetValueAsUnsigned()
            size = r.GetChildMemberWithName("size").GetValueAsUnsigned()
            data = r.GetChildMemberWithName("data").GetValueAsUnsigned()
            self.regions.append((base, size, data))
        self.process = lldb.debugger.GetSelectedTarget().GetProcess()
        self.used = self.mem.GetChildMemberWithName("used").GetValueAsUnsigned()
        self.limit = self.mem.GetChildMemberWithName("limit").GetValueAsUnsigned()

    def find(self, addr, n=1):
        for base, size, data in self.regions:
            if addr >= base and addr - base + n <= size:
                return data + (addr - base), size - (addr - base)
        return None, 0

    def read(self, addr, n):
        host, avail = self.find(addr, n)
        if host is None:
            raise RuntimeError("guest %08x+%d not mapped" % (addr, n))
        err = lldb.SBError()
        buf = self.process.ReadMemory(host, n, err)
        if not err.Success():
            raise RuntimeError("host read failed: %s" % err)
        return buf

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def u16(self, addr):
        return struct.unpack("<H", self.read(addr, 2))[0]

    def u8(self, addr):
        return self.read(addr, 1)[0]

    def dwords(self, addr, n):
        return struct.unpack("<%dI" % n, self.read(addr, 4 * n))

    def cstr(self, addr, maxlen=128):
        b = self.read(addr, maxlen)
        z = b.find(b"\0")
        return b[:z if z >= 0 else maxlen].decode("latin-1")


def find_run_frame():
    t = lldb.debugger.GetSelectedTarget()
    for th in t.GetProcess():
        for f in th:
            name = f.GetFunctionName() or ""
            if name == "run" or name.endswith("!run"):
                return f
    raise RuntimeError("run() frame not found; adjust breakpoint")


def hexdump(g, addr, length, row=16):
    data = g.read(addr, length)
    for off in range(0, length, row):
        chunk = data[off:off + row]
        hx = " ".join("%02x" % c for c in chunk)
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        out("  %08x  %-47s  %s" % (addr + off, hx, asc))


def dump_struct(g, addr, n, label="object"):
    out("%s at %08x:" % (label, addr))
    vals = g.dwords(addr, n)
    for i, v in enumerate(vals):
        off = i * 4
        note = ""
        if 0x10000000 <= v < 0x70000000:
            note = "  -> guest %08x" % v
            h, avail = g.find(v, 1)
            if h is not None:
                try:
                    s = g.cstr(v, 64)
                    if s and all(32 <= ord(c) < 127 for c in s):
                        note += ' "%s"' % s
                except Exception:
                    pass
        out("  +%03x: %08x%s" % (off, v, note))


def nonzero_pages(g, base, size, stride=1, chunk=0x10000):
    """Count 4 KiB pages with at least one non-zero byte.

    stride: sample every Nth page (1 = all). Returns (nonzero, sampled, bytes_read).
    """
    total = size // 0x1000
    sampled = 0
    nonzero = 0
    read_bytes = 0
    p = 0
    while p < total:
        page_indexes = list(range(p, min(p + chunk // 0x1000, total), stride))
        if not page_indexes:
            break
        first = page_indexes[0]
        last = page_indexes[-1]
        span = (last - first) * 0x1000 + 0x1000
        data = g.read(base + first * 0x1000, span)
        read_bytes += span
        for idx in page_indexes:
            off = (idx - first) * 0x1000
            page = data[off:off + 0x1000]
            sampled += 1
            if any(page):
                nonzero += 1
        p += chunk // 0x1000
    return nonzero, sampled, read_bytes


def main(debugger, command, result, internal_dict):
    frame = find_run_frame()
    g = Guest(frame)
    args = command.split()
    stride = int(args[0]) if args else 16

    out("=== GuestMemory: regions=%d used=%d (%.1f MiB) limit=%d (%.1f MiB)"
        % (g.count, g.used, g.used / 1048576.0, g.limit, g.limit / 1048576.0))
    for i, (base, size, data) in enumerate(g.regions):
        tag = ""
        if base <= POOL_OBJ < base + size:
            tag = "  <-- contains 1b418b0"
        if base <= ARENA_BASE < base + size:
            tag = "  <-- ARENA"
        out("  [%2d] %08x + %9d (%.3f MiB) host=%016x%s"
            % (i, base, size, size / 1048576.0, data, tag))

    out()
    out("=== global allocator 0x%08x ===" % POOL_OBJ)
    dump_struct(g, POOL_OBJ, 0xD0)
    cell = g.u32(POOL_OBJ + 0x08)
    out("  [0x1b418b0+0x08] = %08x  (12-byte cell)" % cell)
    if cell:
        vals = g.dwords(cell, 3)
        out("  cell[0] Default Heap = %08x" % vals[0])
        out("  cell[1] Static  Heap = %08x" % vals[1])
        out("  cell[2] File    Heap = %08x" % vals[2])
    p18 = g.u32(POOL_OBJ + 0x18)
    out("  [0x1b418b0+0x18] = %08x" % p18)
    if p18:
        for name, off in (("vtable", 0x00), ("+0x04", 0x04), ("name", 0x10),
                          ("buckets", 0x14), ("granularity", 0x18),
                          ("reserve", 0x20), ("commit_hi", 0x24), ("committed", 0x28),
                          ("base", 0x3c), ("+0x40", 0x40), ("+0x44", 0x44),
                          ("+0x48", 0x48), ("+0x4c", 0x4c), ("flag50", 0x50),
                          ("flag51", 0x51), ("flag154", 0x154)):
            v = g.u32(p18 + off)
            extra = ""
            if name == "name" and v:
                try:
                    extra = '  "%s"' % g.cstr(v, 64)
                except Exception:
                    pass
            out("  pool.%-12s +%03x = %08x (%.3f MiB)%s"
                % (name, off, v, v / 1048576.0 if off in (0x20, 0x24, 0x28) else 0, extra))
        out("  pool free lists +0x54..+0xd3 (small bins):")
        out("    " + " ".join("%08x" % v for v in g.dwords(p18 + 0x54, 32)))
        out("  pool free lists +0xd4..+0x153 (large bins):")
        out("    " + " ".join("%08x" % v for v in g.dwords(p18 + 0xD4, 32)))
        out("  pool 0x158 block:")
        hexdump(g, p18, 0x158)

    out()
    out("=== [0x1bbeb54] and fs:[0x2c] ===")
    try:
        cpu = frame.FindVariable("b").GetChildMemberWithName("cpu")
        fs = cpu.GetChildMemberWithName("fs_base").GetValueAsUnsigned()
    except Exception:
        fs = 0
    out("  guest fs_base = %08x" % fs)
    out("  [0x1bbeb54] (TLS index) = %08x" % g.u32(0x1BBEB54))
    out("  [0x1b418a0] = %08x" % g.u32(0x1B418A0))
    out("  [0x1b2e860] (current thread obj) = %08x" % g.u32(0x1B2E860))
    if fs:
        try:
            tlsarr = g.u32(fs + 0x2C)
            out("  fs:[0x2c] (TLS pointer array) = %08x" % tlsarr)
            idx = g.u32(0x1BBEB54)
            if tlsarr and idx < 64:
                out("  TLS[%d] = %08x" % (idx, g.u32(tlsarr + 4 * idx)))
            out("  TEB.ThreadId fs:[0x24] = %08x" % g.u32(fs + 0x24))
        except Exception as e:
            out("  fs:[0x2c] unreadable: %s" % e)
    th = g.u32(0x1B2E860)
    if th:
        dump_struct(g, th, 0x20, "thread object")

    out()
    out("=== arena pages ===")
    # regions that lie inside the arena reservation (256 MiB from 0x40000000)
    arena_regs = [(b, s, d) for (b, s, d) in g.regions if ARENA_BASE <= b < ARENA_BASE + 0x10000000]
    committed = sum(s for (b, s, d) in arena_regs)
    out("  committed regions in arena: %d, %d bytes (%.2f MiB)"
        % (len(arena_regs), committed, committed / 1048576.0))
    nz = samp = rb = 0
    for b, s, d in arena_regs:
        a, c, r = nonzero_pages(g, b, s, stride)
        nz += a
        samp += c
        rb += r
    out("  sampled every %dth page: %d/%d pages non-zero (%.1f%%), read %.2f MiB"
        % (stride, nz, samp, 100.0 * nz / samp if samp else 0, rb / 1048576.0))
    out("  estimated touched: %.2f MiB of %.2f MiB (sampling error +-%.2f MiB at 95%%)"
        % (nz * 4096 / 1048576.0, committed / 1048576.0,
           1.96 * ((samp * 0.25) ** 0.5) * 4096 / 1048576.0 if samp else 0))

    # last bytes of each arena region, to see where writes stopped
    out()
    out("=== per-region non-zero pages (stride 1, bounded to 1.75 MiB each) ===")
    for b, s, d in arena_regs:
        a, c, r = nonzero_pages(g, b, s, 1)
        out("  %08x %8d B: %5d/%5d pages non-zero (%.1f%%)"
            % (b, s, a, c, 100.0 * a / c if c else 0))


STATE = {"ctors": [], "pools": []}


def _reg(frame, name):
    return frame.FindRegister(name).GetValueAsUnsigned()


def _cpu_r(c_val, index):
    r = c_val.GetChildMemberWithName("r")
    return r.GetChildAtIndex(index).GetValueAsUnsigned()


def watch(frame, bp_loc, internal_dict):
    """Breakpoint callback: log the pool constructor's `this` (guest ecx)."""
    c_ptr = _reg(frame, "rcx")
    name = frame.GetFunctionName() or ""
    if c_ptr:
        # CPU* is the only argument; read CPU.r[1] (ecx) through the debuggee.
        err = lldb.SBError()
        proc = lldb.debugger.GetSelectedTarget().GetProcess()
        blob = proc.ReadMemory(c_ptr, 8 * 4 + 0x40, err)
        if err.Success():
            r1 = struct.unpack_from("<I", blob, 4)[0]
            STATE["ctors"].append(r1)
            out("  [watch] %s this(ecx)=%08x hit#%d" % (name, r1, len(STATE["ctors"])))
    return False  # do not stop


def watch_args(frame, bp_loc, internal_dict):
    """Log guest stdcall args (esp+4, esp+8) of a lifted function."""
    try:
        g = Guest(find_run_frame())
        c_ptr = _reg(frame, "rcx")
        err = lldb.SBError()
        proc = lldb.debugger.GetSelectedTarget().GetProcess()
        blob = proc.ReadMemory(c_ptr, 8 * 4, err)
        if not err.Success():
            return False
        esp = struct.unpack_from("<I", blob, 4 * 4)[0]
        a0 = g.u32(esp + 4) if g.find(esp + 4, 4)[0] else 0xDEAD
        a1 = g.u32(esp + 8) if g.find(esp + 8, 4)[0] else 0xDEAD
        name = frame.GetFunctionName() or ""
        out("  [args] %s esp=%08x arg0=%08x arg1=%08x" % (name, esp, a0, a1))
    except Exception as e:
        out("  [args] error: %s" % e)
    return False


def install(debugger, command, result, internal_dict):
    t = debugger.GetSelectedTarget()
    for fn in command.split() or ["f_687E20"]:
        bp = t.BreakpointCreateByName(fn)
        bp.SetScriptCallbackFunction("arena_dump.watch")
        out("breakpoint on %s -> %s" % (fn, bp))


def install_args(debugger, command, result, internal_dict):
    t = debugger.GetSelectedTarget()
    for fn in command.split():
        bp = t.BreakpointCreateByName(fn)
        bp.SetScriptCallbackFunction("arena_dump.watch_args")
        out("args breakpoint on %s -> %s" % (fn, bp))


def patch_commit_arg(frame, bp_loc, internal_dict):
    """Rewrite the Default Heap initial-commit argument on the guest stack.

    f_A4DE80(this, reserve, commit) is the pool's reserve+commit vtable entry.
    The guest reads arg1 from [esp+8]; changing it here measures what happens
    with a smaller (or zero = fully lazy) initial commit. No file is modified.
    """
    if STATE.get("patched"):
        return False
    try:
        g = Guest(find_run_frame())
        c_ptr = _reg(frame, "rcx")
        err = lldb.SBError()
        proc = lldb.debugger.GetSelectedTarget().GetProcess()
        blob = proc.ReadMemory(c_ptr, 8 * 4, err)
        esp = struct.unpack_from("<I", blob, 4 * 4)[0]
        reserve = g.u32(esp + 4)
        commit = g.u32(esp + 8)
        new = STATE["patch"]
        host, _ = g.find(esp + 8, 4)
        proc.WriteMemory(host, struct.pack("<I", new), err)
        STATE["patched"] = True
        out("  [patch] f_A4DE80 reserve=%08x commit=%08x -> commit=%08x (%.3f MiB)"
            % (reserve, commit, new, new / 1048576.0))
    except Exception as e:
        out("  [patch] error: %s" % e)
    return False


def install_patch(debugger, command, result, internal_dict):
    t = debugger.GetSelectedTarget()
    STATE["patch"] = int(command.strip() or "0", 0)
    bp = t.BreakpointCreateByName("f_A4DE80")
    bp.SetScriptCallbackFunction("arena_dump.patch_commit_arg")
    out("patch breakpoint on f_A4DE80 -> commit=%x" % STATE["patch"])


def watch_native(frame, bp_loc, internal_dict):
    """Log a win32 shim's guest stdcall arguments (CPU* in rcx, args on the guest stack)."""
    try:
        name = frame.GetFunctionName() or ""
        c_ptr = _reg(frame, "rcx")
        proc = lldb.debugger.GetSelectedTarget().GetProcess()
        err = lldb.SBError()
        blob = proc.ReadMemory(c_ptr, 8 * 4, err)
        if not err.Success():
            return False
        esp = struct.unpack_from("<I", blob, 4 * 4)[0]
        g = Guest(find_run_frame())
        a = []
        for i in range(4):
            if g.find(esp + 4 + 4 * i, 4)[0]:
                a.append(g.u32(esp + 4 + 4 * i))
            else:
                a.append(0xDEADBEEF)
        out("  [native] %s(%08x, %08x, %08x, %08x)" % (name, *a))
    except Exception as e:
        out("  [native] error: %s" % e)
    return False


def install_native(debugger, command, result, internal_dict):
    t = debugger.GetSelectedTarget()
    bp = t.BreakpointCreateByName(command.strip())
    bp.SetScriptCallbackFunction("arena_dump.watch_native")
    out("native breakpoint on %s -> %s" % (command.strip(), bp))


def watch_native_plain(frame, bp_loc, internal_dict):
    name = frame.GetFunctionName() or ""
    args = [_reg(frame, r) for r in ("rcx", "rdx", "r8", "r9")]
    out("  [np] %s(%08x, %08x, %08x, %08x)" % (name, *args))
    return False


def install_native_plain(debugger, command, result, internal_dict):
    t = debugger.GetSelectedTarget()
    bp = t.BreakpointCreateByName(command.strip())
    bp.SetScriptCallbackFunction("arena_dump.watch_native_plain")
    out("plain native breakpoint on %s -> %s" % (command.strip(), bp))


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f arena_dump.main arena_dump")
    debugger.HandleCommand("command script add -f arena_dump.install arena_watch")
    debugger.HandleCommand("command script add -f arena_dump.install_args arena_args")
    debugger.HandleCommand("command script add -f arena_dump.install_patch arena_patcharg")
    debugger.HandleCommand("command script add -f arena_dump.install_native arena_native")
    debugger.HandleCommand("command script add -f arena_dump.install_native_plain arena_np")
