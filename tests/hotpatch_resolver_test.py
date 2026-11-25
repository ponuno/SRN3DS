"""Unit tests for the MSVC lazy-ILT thunk resolver in prepare_pilot.py.

Run with the project venv:  .venv/Scripts/python.exe tests/hotpatch_resolver_test.py
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import prepare_pilot as pp  # noqa: E402

BASE = 0x10000000
SLOT = BASE + 0x2000
ROOT = BASE + 0x1100
INIT = BASE + 0x1400
PROLOGUE = b"\x8b\xff\x55\x8b\xec"


def w32(image, addr, value):
    struct.pack_into("<I", image, addr - BASE, value)


def make_image(kind="jmp", body=b"\x8b\xff\x55\x8b\xec\xc3", pad=True,
               wrapper_prologue=PROLOGUE, initializer=True, forward=True):
    image = bytearray(0x3000)
    body_off = ROOT + 11 - BASE
    image[body_off:body_off + len(body)] = body
    image[ROOT - BASE:ROOT - BASE + 2] = b"\xff\x25"
    w32(image, ROOT + 2, SLOT)
    if pad:
        image[ROOT - BASE + 6:ROOT - BASE + 11] = b"\xcc" * 5
    wrapper = bytearray(wrapper_prologue)
    if initializer:
        wrapper += b"\x6a\x01\xe8\x00\x00\x00\x00"
    else:
        wrapper += b"\x6a\x01\x90\x90"
    forward_bytes = b""
    if forward:
        if kind == "jmp":
            forward_bytes = b"\xff\x25" + struct.pack("<I", SLOT)
        else:
            forward_bytes = b"\xff\x15" + struct.pack("<I", SLOT) + b"\x5d\xc2\x10\x00"
    # layout: [prologue+call][forward][int3 x5] directly before the export thunk
    raw_len = len(wrapper) + len(forward_bytes) + 5
    wrapper_addr = ROOT - raw_len
    if initializer:
        struct.pack_into("<i", wrapper, 8, (INIT - (wrapper_addr + 12)) & 0xffffffff)
    wrapper += forward_bytes
    wrapper += b"\xcc" * 5
    image[wrapper_addr - BASE:wrapper_addr - BASE + len(wrapper)] = wrapper
    w32(image, SLOT, wrapper_addr)
    return bytes(image), wrapper_addr


def expect_ok(label, image, **expected):
    got = pp.resolve_ilt_thunk(image, BASE, ROOT)
    assert got is not None, f"{label}: expected resolution, got None"
    for key, value in expected.items():
        assert got[key] == value, f"{label}: {key}={got[key]:#x} expected {value:#x}"
    print(f"  ok: {label}")


def expect_reject(label, image, needle):
    try:
        pp.resolve_ilt_thunk(image, BASE, ROOT)
    except ValueError as exc:
        assert needle in str(exc), f"{label}: message {exc!r} lacks {needle!r}"
        print(f"  ok: {label} -> {exc}")
        return
    raise AssertionError(f"{label}: expected ValueError")


def main():
    print("hotpatch resolver tests")
    image, wrapper = make_image("jmp")
    expect_ok("jmp thunk", image, slot=SLOT, wrapper=wrapper, init=INIT, real=ROOT + 11)

    image, wrapper = make_image("call")
    expect_ok("call thunk", image, slot=SLOT, wrapper=wrapper, init=INIT, real=ROOT + 11)

    plain = bytearray(0x3000)
    plain[ROOT - BASE:ROOT - BASE + 6] = PROLOGUE + b"\xc3"
    assert pp.resolve_ilt_thunk(bytes(plain), BASE, ROOT) is None
    print("  ok: plain export returns None")
    assert pp.resolve_ilt_thunk(bytes(plain), BASE, BASE + 2) is None
    print("  ok: export below thunk size returns None")

    expect_reject("missing int3 padding", make_image(pad=False)[0], "int3 padding")
    expect_reject("bad wrapper prologue", make_image(wrapper_prologue=b"\x90\x90\x90\x90\x90\x90")[0], "frame prologue")
    expect_reject("no initializer call", make_image(initializer=False)[0], "one-time initializer")
    expect_reject("no slot forwarding", make_image(forward=False)[0], "does not forward")
    expect_reject("bad real body", make_image(body=b"\x90\x90\x90\x90\x90\xc3")[0], "real body lacks")

    image, wrapper = make_image("jmp")
    broken = bytearray(image)
    w32(broken, SLOT, BASE + 0x0A00)  # zero-filled address before the export
    expect_reject("slot target without prologue", bytes(broken), "frame prologue")

    print("hotpatch resolver: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
