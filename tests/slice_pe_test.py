"""Unit tests for tools/slice_pe.py local function decoding.

Run with the project venv:  .venv/Scripts/python.exe tests/slice_pe_test.py
"""
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import slice_pe as sp  # noqa: E402

BASE = 0x1000
IMAGE_LEN = 0x4000
EXEC = [(BASE, BASE + 0x1000)]


def make_image(size=IMAGE_LEN):
    return bytearray(size)


def put(image, addr, data):
    image[addr - BASE:addr - BASE + len(data)] = data


def md32():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


def decode(image, start, budget=4096):
    return sp.build_function_blocks(md32(), bytes(image), BASE, EXEC, start,
                                    budget=budget)


def expect_blocks(label, blocks, expected):
    assert blocks == expected, f"{label}: blocks={[(hex(a), hex(b)) for a, b in blocks]} " \
                               f"expected {[(hex(a), hex(b)) for a, b in expected]}"
    print(f"  ok: {label}")


def test_branch_and_ret():
    image = make_image()
    put(image, 0x1000, bytes.fromhex("85c0"))        # test eax, eax
    put(image, 0x1002, bytes.fromhex("7406"))        # je 0x100a
    put(image, 0x1004, bytes.fromhex("90"))          # nop
    put(image, 0x1005, bytes.fromhex("31c0"))        # xor eax, eax
    put(image, 0x1007, bytes.fromhex("40"))          # inc eax
    put(image, 0x1008, bytes.fromhex("eb02"))        # jmp 0x100c
    put(image, 0x100a, bytes.fromhex("89d8"))        # mov eax, ebx
    put(image, 0x100c, bytes.fromhex("c3"))          # ret
    blocks, tables, stats = decode(image, 0x1000)
    expect_blocks("conditional branch + jmp + ret", blocks,
                  [(0x1000, 0x1004), (0x1004, 0x100a),
                   (0x100a, 0x100c), (0x100c, 0x100d)])
    assert not tables and not stats["truncated"] and stats["failed"] == []
    print("  ok: no jump tables, no truncation")


def test_call_fallthrough_not_followed():
    image = make_image()
    # call 0x2300 (outside the executable range), then ret.
    put(image, 0x1300, bytes.fromhex("e8fb0f0000"))
    put(image, 0x1305, bytes.fromhex("c3"))
    blocks, _, _ = decode(image, 0x1300)
    expect_blocks("call ends block and fallthrough is kept", blocks,
                  [(0x1300, 0x1305), (0x1305, 0x1306)])


def test_padding_stops():
    image = make_image()
    put(image, 0x1000, bytes.fromhex("c3"))
    put(image, 0x1001, b"\xcc" * 8)
    blocks, _, _ = decode(image, 0x1000)
    expect_blocks("int3 padding is not decoded", blocks, [(0x1000, 0x1001)])
    blocks, _, _ = decode(image, 0x1001)
    expect_blocks("decode starting in int3 padding", blocks, [])

    image = make_image()
    put(image, 0x1100, bytes.fromhex("c3"))
    put(image, 0x1101, b"\x90\x90\x90")
    blocks, _, _ = decode(image, 0x1101)
    expect_blocks("nop padding run is not decoded", blocks, [])


def test_budget():
    image = make_image()
    put(image, 0x1200, bytes.fromhex("40404040c3"))  # inc eax x4, ret
    blocks, _, stats = decode(image, 0x1200, budget=3)
    expect_blocks("instruction budget truncates", blocks, [(0x1200, 0x1203)])
    assert stats["truncated"], "budget test: expected truncated flag"
    print("  ok: truncated flag set")


def test_jump_table():
    image = make_image()
    # jmp dword ptr [eax*4 + 0x4000]
    put(image, 0x1400, bytes.fromhex("ff248500400000"))
    put(image, 0x1410, bytes.fromhex("b801000000c3"))  # mov eax, 1; ret
    put(image, 0x1416, bytes.fromhex("b802000000c3"))  # mov eax, 2; ret
    struct.pack_into("<III", image, 0x3000, 0x1410, 0x1416, 0)
    blocks, tables, _ = decode(image, 0x1400)
    assert tables == {0x1400: [0x1410, 0x1416]}, f"jump table: {tables}"
    starts = {lo for lo, _ in blocks}
    assert {0x1410, 0x1416} <= starts, f"jump targets missing from blocks: {starts}"
    print("  ok: jump table targets become blocks")

    # A table whose targets leave the function is treated as data/callback
    # pointers, not as a switch table.
    blocks, tables, _ = sp.build_function_blocks(
        md32(), bytes(image), BASE, EXEC, 0x1400, jump_table_span=0x14)
    assert tables == {}, f"far jump table should be rejected: {tables}"
    print("  ok: out-of-range jump table rejected")


def test_executable_ranges_and_root_file():
    class FakeSection:
        def __init__(self, va, vsize, rawsize, characteristics):
            self.VirtualAddress = va
            self.Misc_VirtualSize = vsize
            self.SizeOfRawData = rawsize
            self.Characteristics = characteristics

    class FakePE:
        sections = [FakeSection(0x1000, 0x500, 0x600, 0x60000020),
                    FakeSection(0x2000, 0x100, 0x200, 0x40000040)]

    ranges = sp.executable_ranges(FakePE(), 0x400000, 0x3000)
    assert ranges == [(0x401000, 0x401500)], ranges
    print("  ok: only execute sections become ranges")

    text = ("# TLS callbacks\n0xf794ab\n0x000F5B303  # second\n\nf58b99\n"
            "0x1234h\r\n")
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "roots.txt"
        path.write_text(text, encoding="utf-8")
        roots = sp.read_root_file(path)
    assert roots == [0xF794AB, 0xF5B303, 0xF58B99, 0x1234], [hex(r) for r in roots]
    print("  ok: extra-root-file parsing")


def test_admission_budget_and_closure():
    image = make_image()
    funcs = {0x1000: [(0x1000, 0x1001)],
             0x1001: [(0x1001, 0x1002)],
             0x1002: [(0x1002, 0x1003)]}

    admission = sp.Admission(funcs, {}, EXEC, bytes(image), BASE, 2)
    assert admission.admit(0x1000), "first function should be admitted"
    assert admission.admit(0x1001), "second function should be admitted"
    assert not admission.admit(0x1002), "third function exceeds the budget"
    assert admission.admitted == {0x1000, 0x1001}, admission.admitted
    assert list(admission.queue) == [0x1000, 0x1001], list(admission.queue)
    assert admission.cut == {0x1002: "function budget"}, admission.cut
    print("  ok: enqueue-time budget cut is recorded")

    edges = {0x1000: {0x1001}, 0x1001: {0x1002}}
    kept, dropped = sp.prune_to_closure(admission.admitted, edges,
                                        lambda target: target in funcs)
    assert kept == set(), kept
    assert dropped == {0x1000, 0x1001}, dropped
    keep_all, drop_none = sp.prune_to_closure({0x1000, 0x1001, 0x1002}, edges,
                                              lambda target: target in funcs)
    assert keep_all == {0x1000, 0x1001, 0x1002}
    assert drop_none == set()
    print("  ok: pruning cascades to a closed set, succeeds when it fits")

    nonroot_edges = {0x1001: {0x1002}}
    kept, dropped = sp.prune_to_closure(admission.admitted, nonroot_edges,
                                        lambda target: target in funcs)
    assert kept == {0x1000}, kept
    assert dropped == {0x1001}, dropped

    root_edges = {0x1000: {0x1002}}
    kept, dropped = sp.prune_to_closure({0x1000}, root_edges,
                                        lambda target: target in funcs)
    assert kept == set(), kept
    assert dropped == {0x1000}, dropped
    message = sp.budget_error_message(admission.admitted, admission.cut, 2)
    assert "budget too small for a closed slice; need >= 3 functions" in message, message
    print("  ok: root pruning produces the explicit budget error")

    iat = {0x3000: "KERNEL32.dll!Test"}
    put(image, 0x1004, bytes.fromhex("ff2500300000"))  # jmp dword ptr [0x3000]
    iat_admission = sp.Admission({0x1004: []}, iat, EXEC, bytes(image), BASE, 8)
    assert not iat_admission.admit(0x1004), "IAT thunks must not be admitted"
    assert iat_admission.rejected["iat thunk"] == 1
    print("  ok: IAT thunks are rejected at admission")


def test_data_slot_runs():
    image = make_image()
    funcs = set()
    for index in range(10):
        addr = 0x1010 + index * 0x10
        put(image, addr, bytes.fromhex("558becc3"))
        funcs.add(addr)
    put(image, 0x1100, bytes.fromhex("558becc3"))   # push ebp; mov ebp, esp; ret
    put(image, 0x1200, bytes.fromhex("8bff558becc3"))  # mov edi, edi; ...
    put(image, 0x1300, bytes.fromhex("83ec10c3"))   # sub esp, 0x10; ret
    put(image, 0x1400, bytes.fromhex("c3"))         # ret: not a function prologue
    funcs.update({0x1100, 0x1200, 0x1300, 0x1400})
    data = [(0x3000, 0x4000)]

    struct.pack_into("<IIII", image, 0x2000, 0x1100, 0x1200, 0x1300, 0)
    run = sp.data_slot_run(0x3000, bytes(image), BASE, EXEC, funcs, data, {})
    assert run == [0x1100, 0x1200, 0x1300], [hex(a) for a in run]
    print("  ok: data-slot run accepts valid function starts")

    struct.pack_into("<III", image, 0x2000, 0x1100, 0x1400, 0x1200)
    run = sp.data_slot_run(0x3000, bytes(image), BASE, EXEC, funcs, data, {})
    assert run == [0x1100], [hex(a) for a in run]
    print("  ok: data-slot run stops at a non-function value")

    struct.pack_into("<I", image, 0x2000, 0x1400)
    run = sp.data_slot_run(0x3000, bytes(image), BASE, EXEC, funcs, data, {})
    assert run == [], [hex(a) for a in run]
    print("  ok: non-function value rejected")

    struct.pack_into("<I", image, 0x2000, 0)
    assert sp.data_slot_run(0x3000, bytes(image), BASE, EXEC, funcs, data, {}) == []
    struct.pack_into("<I", image, 0x2000, 0xFFFFFFFF)
    assert sp.data_slot_run(0x3000, bytes(image), BASE, EXEC, funcs, data, {}) == []
    print("  ok: zero and all-ones values rejected")

    ascii_value = 0x31323334  # "4321"
    wide_exec = [(BASE, 0x40000000)]
    ascii_funcs = set(funcs) | {ascii_value}
    struct.pack_into("<I", image, 0x2000, ascii_value)
    assert sp.data_slot_run(0x3000, bytes(image), BASE, wide_exec, ascii_funcs,
                            data, {}) == []
    print("  ok: four printable ASCII bytes rejected")

    struct.pack_into("<I", image, 0x2000, 0x1200)
    assert sp.data_slot_run(0x3000, bytes(image), BASE, EXEC, {0x1100}, data, {}) == []
    print("  ok: value outside discovery functions rejected")

    struct.pack_into("<I", image, 0x2000, 0x1100)
    assert sp.data_slot_run(0x3000, bytes(image), BASE, EXEC, funcs,
                            [(0x3100, 0x3200)], {}) == []
    assert sp.data_slot_run(0x3000, bytes(image), BASE, EXEC, funcs, data,
                            {0x3000: "IAT"}) == []
    print("  ok: slot outside .data/.rdata or in the IAT rejected")

    run_values = [0x1010 + index * 0x10 for index in range(10)]
    struct.pack_into("<" + "I" * len(run_values), image, 0x2000, *run_values)
    run = sp.data_slot_run(0x3000, bytes(image), BASE, EXEC, funcs, data, {})
    assert run == run_values[:sp.MAX_DATA_RUN], [hex(a) for a in run]
    print("  ok: data-slot run is capped at MAX_DATA_RUN")

    struct.pack_into("<IIIII", image, 0x2000, 0, 0, 0x1100, 0x1200, 0)
    run = sp.data_table_run(0x3000, bytes(image), BASE, EXEC, funcs, data, {})
    assert run == [0x1100, 0x1200], [hex(a) for a in run]
    print("  ok: data-table run tolerates two leading NULLs")

    # Immediate-referenced vtables may contain short virtual methods such as
    # a bare `ret`; discovered membership is enough for a data table.
    image = make_image()
    put(image, 0x1100, bytes.fromhex("558becc3"))
    put(image, 0x1400, bytes.fromhex("c3"))
    short_funcs = {0x1100, 0x1400}
    struct.pack_into("<II", image, 0x2000, 0x1100, 0x1400)
    run = sp.data_table_run(0x3000, bytes(image), BASE, EXEC, short_funcs, data, {})
    assert run == [0x1100, 0x1400], [hex(a) for a in run]
    print("  ok: immediate data tables accept short virtual methods")


def test_tls_callbacks():
    class FakeDirectory:
        def __init__(self, va, size):
            self.VirtualAddress = va
            self.Size = size

    class FakeOptional:
        def __init__(self, directories):
            self.DATA_DIRECTORY = directories

    class FakePE:
        def __init__(self, directories):
            self.OPTIONAL_HEADER = FakeOptional(directories)

    image = make_image()
    directories = [FakeDirectory(0, 0) for _ in range(10)]
    directories[sp.IMAGE_DIRECTORY_ENTRY_TLS] = FakeDirectory(0x1000, 24)
    pe = FakePE(directories)
    struct.pack_into("<IIIIII", image, 0x1000, 0, 0, 0, 0x3000, 0, 0)

    struct.pack_into("<IIIII", image, 0x2000, 0, 0, 0x1100, 0x1200, 0)
    roots = sp.tls_callback_roots(pe, BASE, bytes(image))
    assert roots == [0x1100, 0x1200], [hex(a) for a in roots]
    print("  ok: TLS array skips two leading NULLs")

    struct.pack_into("<IIII", image, 0x2000, 0x1100, 0, 0x1200, 0)
    roots = sp.tls_callback_roots(pe, BASE, bytes(image))
    assert roots == [0x1100], [hex(a) for a in roots]
    print("  ok: TLS array stops at the first post-entry NULL")

    struct.pack_into("<IIIII", image, 0x2000, 0, 0, 0, 0x1100, 0)
    roots = sp.tls_callback_roots(pe, BASE, bytes(image))
    assert roots == [], [hex(a) for a in roots]
    print("  ok: more than two leading NULLs ends the TLS array")

    values = [0x1100 + index * 0x10 for index in range(17)]
    struct.pack_into("<" + "I" * len(values), image, 0x2000, *values)
    roots = sp.tls_callback_roots(pe, BASE, bytes(image))
    assert roots == values[:sp.TLS_MAX_CALLBACKS], [hex(a) for a in roots]
    print("  ok: TLS callbacks are capped")

    directories[sp.IMAGE_DIRECTORY_ENTRY_TLS] = FakeDirectory(0, 0)
    assert sp.tls_callback_roots(pe, BASE, bytes(image)) == []
    print("  ok: missing TLS directory yields no roots")


def write_synthetic_pe(path):
    """Write a tiny PE32 with one .text section at VA 0x401000."""
    buf = bytearray(0x1400)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x40)
    buf[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", buf, 0x44,
                     0x14C, 1, 0, 0, 0, 0xE0, 0x0102)
    opt = 0x58
    struct.pack_into("<HBBIIIIII", buf, opt,
                     0x10B, 0, 0, 0x1000, 0, 0, 0x1000, 0x1000, 0x2000)
    struct.pack_into("<I", buf, opt + 28, 0x400000)
    struct.pack_into("<II", buf, opt + 32, 0x1000, 0x200)
    struct.pack_into("<HHHHHH", buf, opt + 40, 4, 0, 0, 0, 4, 0)
    struct.pack_into("<I", buf, opt + 52, 0)
    struct.pack_into("<II", buf, opt + 56, 0x2000, 0x400)
    struct.pack_into("<I", buf, opt + 64, 0)
    struct.pack_into("<HH", buf, opt + 68, 3, 0)
    struct.pack_into("<IIII", buf, opt + 72,
                     0x100000, 0x1000, 0x100000, 0x1000)
    struct.pack_into("<II", buf, opt + 88, 0, 16)
    section = 0x138
    buf[section:section + 8] = b".text\0\0\0"
    struct.pack_into("<IIIIIIHHI", buf, section + 8,
                     0x1000, 0x1000, 0x1000, 0x400, 0, 0, 0, 0, 0x60000020)
    # 0x401000: call 0x401010; ret.  0x401010: ret.
    buf[0x400:0x406] = bytes.fromhex("e80b000000c3")
    buf[0x410] = 0xC3
    Path(path).write_bytes(buf)


def test_allow_open_cli():
    tools = Path(__file__).resolve().parent.parent / "tools" / "slice_pe.py"
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        exe = tmp / "synthetic.exe"
        write_synthetic_pe(exe)
        functions = {
            "image_base": 0x400000,
            "entry": 0x401000,
            "functions": {"0x401000": [[0x401000, 0x401006]]},
            "iat": {},
            "jump_tables": {},
            "indirect_jumps": [],
        }
        functions_path = tmp / "functions.json"
        functions_path.write_text(json.dumps(functions), encoding="utf-8")

        base_cmd = [sys.executable, str(tools), str(exe), str(functions_path),
                    "--root", "entry", "--max-functions", "1",
                    "--max-insns", "1000", "--emit"]
        strict_out = tmp / "strict"
        strict = subprocess.run(base_cmd + ["--out", str(strict_out)],
                                capture_output=True, text=True, timeout=60)
        strict_text = strict.stdout + strict.stderr
        assert strict.returncode == 2, (strict.returncode, strict_text)
        assert "budget too small for a closed slice; need >= 2 functions" in strict_text, \
            strict_text
        assert not (strict_out / "slice_functions.json").exists()
        assert not (strict_out / "slice_report.json").exists()
        print("  ok: default mode refuses a root-dropping budget")

        open_out = tmp / "open"
        allowed = subprocess.run(base_cmd + ["--allow-open", "--out", str(open_out)],
                                 capture_output=True, text=True, timeout=60)
        allowed_text = allowed.stdout + allowed.stderr
        assert allowed.returncode == 0, (allowed.returncode, allowed_text)
        assert "open slice: 1 admitted, 1 cut targets" in allowed_text, allowed_text
        assert "first executed cut target will fault" in allowed_text, allowed_text

        emitted = json.loads((open_out / "slice_functions.json").read_text(encoding="utf-8"))
        assert set(emitted["functions"]) == {"0x401000"}, emitted["functions"].keys()
        report = json.loads((open_out / "slice_report.json").read_text(encoding="utf-8"))
        assert report["mode"] == "open", report["mode"]
        assert report["closed"] is False, report["closed"]
        assert report["selected_functions"] == 1, report["selected_functions"]
        assert report["pruned_functions"] == 0, report["pruned_functions"]
        assert "0x401010" in report["cut_targets"], report["cut_targets"]
        print("  ok: --allow-open emits the admitted set with the cut target reported")


def main():
    print("slice_pe local decode tests")
    test_branch_and_ret()
    test_call_fallthrough_not_followed()
    test_padding_stops()
    test_budget()
    test_jump_table()
    test_executable_ranges_and_root_file()
    test_admission_budget_and_closure()
    test_data_slot_runs()
    test_tls_callbacks()
    test_allow_open_cli()
    print("slice_pe: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
