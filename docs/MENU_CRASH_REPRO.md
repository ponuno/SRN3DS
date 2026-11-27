# MENU_CRASH_REPRO - seconds-fast deterministic repro for the `0x60aa70` fault

Status: **host harness, deterministic, verified.** No guest semantics changed.
Worktree `menu-repro`, base `2969b62`.

```
tools\menu_repro.ps1                 # ~3 s warm (< 10 s cold)
tools\menu_repro.ps1 -Regen          # rebuild the mini container first
tools\menu_repro.ps1 -Full           # + newest tesv_slice.exe, fault lines only
```

Successful run prints, among others:

```
FAULT scenario B: [guest] unmapped addr=62783049 bytes=4 last_call=0060aa70 irc_fn=0060aa70 irc_block=0060aa70
TRAP  scenario B: Guest fault at 62783049: unmapped guest memory
FAULT scenario C: [guest] unmapped addr=00000008 bytes=4 last_call=0060aa70 irc_fn=0060aa70 irc_block=0060aa70
HOST PASS: menu_crash_repro
```

The fault text is the runtime's own (`n3ds_guest_ptr` / `rt_unimpl`), so
`addr=` and `last_call=` are produced by the real dispatch path; the test only
constructs the memory/register state.

---

## 1. What `0x60aa70` is

Full disassembly of the retail function (image
`E:\census_ir2\ir_container\_work\ir_container_input.bin`, base `0x401000`;
reproduce with `python tools/world_dis.py 0x60aa70 0x60aa9d`):

```
60aa70  mov  eax,[ecx+8]        ; the crashing load: this->field_8
60aa73  test eax,eax
60aa75  je   60aa7a
60aa77  push eax                ; primary string
60aa78  jmp  60aa7e
60aa7a  mov  ecx,[ecx+0xc]      ; fallback string
60aa7d  push ecx
60aa7e  mov  ecx,[0x1310460]    ; global registry / string map
60aa84  call 0x604750           ; lookup(key) -> record* or NULL
60aa89  test eax,eax
60aa8b  je   60aa9c
60aa8d  mov  cl,[eax+0x12]      ; record tag
60aa90  cmp  cl,0x3c            ; '<'
60aa93  je   60aa9c
60aa95  cmp  cl,0x47            ; 'G'
60aa98  je   60aa9c
60aa9a  xor  eax,eax
60aa9c  ret
```

Contract:

```c
record *check(void *this);   // returns the registry record, or NULL
```

`this` is a value object of at least 16 bytes:

| offset | meaning, from the code |
|---|---|
| `+0x00` | vtable used by the owner/setter (`[vt+0x10]`, `[vt+0x28]`, `[vt+0x2c]`, `[vt+0x40]` in `0x60a960`) |
| `+0x08` | primary NUL-terminated `char *` (the crashing field) |
| `+0x0c` | fallback `char *` used when `+0x8` is NULL |

`0x60aaa0` is the same check restricted to tag `'G'` (and using `+0x8` only);
`0x60a960` is the setter that fills the object:

* `0x60a988` `mov [edi+8], eax` stores the string returned by the source's
  virtual `[vt+0x28]`. This is the **only** write of `+0x8` in the class: if
  the source string is NULL, `+0x8` becomes NULL (not garbage).
* `0x60a9c0..0x60a9f0` fills `+0xc` from the source's second string. If that
  string equals the source's default (`[vt+0x40]`, compared with the global
  null default `0x1b210a8`) it is stored as-is; otherwise it is stored only
  when `lookup(string)` hits, and 0 on a miss.
* `0x60aab5` requires tag `'G'`; `0x60aa90/0x60aa95` accept `'<'` or `'G'`.

Callers (byte-scan `E8 rel32`): `0x5fbb9c`, `0x5fc711`, `0x5fc863`, plus the
indirect paths through the same small class. All three direct sites load

```
mov ecx,[item+0x28]   ; this
call 0x60aa70
```

where `item` is the value of a **12-byte-stride** container node
(`0x5fbb87: add edi,0xc; cmp [edi+8],ebx`, `0x5fc700`): node `+4` is the item,
`+8` is a "next/empty" test. So the crashing pointer is the item's `+0x28`
member, not something computed inside `0x60aa70`.

`0x604750` is a one-argument wrapper:

```
604750  mov eax,[esp+4] ; push eax ; call 0x451a30 ; add esp,4 ; ret 4
```

`0x451a30` hashes the 4-byte key against the lock-protected table
(`0x12e59e4`, buckets at `[0x12e59d4]`) and returns `[node+4]`, the record.
The record's `+0x12` byte is the tag.

### Class name: not recoverable here, honestly

The analysis image is `.text` only (`0x401000..0x106a31a`). It contains no
`.rdata`, so there are no vtables, no RTTI names and no string literals
(`0x1310460`, `0x1b210a8`, `0x107a47c` are all above the image end). The
layout/contract above is exact; the retail class name is not in this image.
This is the same limitation `docs/IR_CONTAINER.md` and `docs/A43F_TRACE.md`
work under.

## 2. The crash and where the garbage can come from

Observed: `ecx = 0x62783041`, `addr = ecx+8 = 0x62783049`,
`last_call = 0x60aa70`, `edx = 0x64556898`, `esi = 0`.

Evidence worth stating:

1. **`0x62783041` is misaligned** (low bit set). Every object pointer in this
   class is at least 4-byte aligned (`mov ecx,[item+0x28]` is a plain load),
   so the value is far more likely corrupted data than a valid object in a
   page the port failed to commit.
2. `0x6278xxxx` lies inside the port's `VirtualAlloc` window
   `0x40000000..0x70000000` (`source/win32_shims.c`), i.e. it *looks like* a
   guest `VirtualAlloc`/CRT-heap address. It is not in the engine's BSMemory
   ranges (`Default Heap` reservation `0x10000000..0x20000000`) nor in a
   `HeapCreate` heap (`0x30000000+`). Reserved-but-uncommitted and
   decommitted pages deliberately fault as "unmapped guest memory", so a real
   but uncommitted pointer would produce exactly this line too.
3. The dword reads as ASCII `A0xb` in memory order (`41 30 78 62`) - string
   bytes, not an address. That also argues for a pointer slot overwritten by
   string data / a struct-layout or store-width mistake.

Hypotheses, ranked by fit with the evidence:

* **H1 - stale/uninitialised `item+0x28`.** The item record came from the
  12-byte container with `+0x28` never written (allocator returned a block
  still holding old bytes). The setter `0x60a960` only ever writes `+0x8` and
  `+0xc`, so `+0x28` itself is the owner's responsibility. Misaligned garbage
  fits this best.
* **H2 - use-after-free / block reuse.** The pre-crash loop is the BSMemory
  multi-heap `try-resize [vt+0x18]` / `allocate [vt+0x10]` / `join 0xa48f11`
  sequence. A wrapper allocated in a heap that was joined/released could still
  be referenced; if the port decommitted the page, the read faults as
  unmapped even though engine state still calls it valid.
* **H3 - layout/offset divergence in the port.** Some path (shim, lift store
  width, or a struct copy) writes a `char *`/string bytes into a pointer slot,
  leaving `0x...41`. The `A0xb` bytes support this but do not prove it.
* **H4 - stale container node.** The 12-byte node itself (`[node+4]`) is stale,
  so `[item+0x28]` is read from freed memory.

The harness does not distinguish H1-H4; see section 4. It pins the *symptom*
(fault address, register, last_call, tier) so any fix can be checked in
seconds.

## 3. The mini container

`tools/menu_repro_pack.py` builds `tests/menu_repro_irc.h` (944-byte
`IRCNT001`) with the **existing** packer (`tests/ir_container_pack.c` + real
`source/ir_decode.c`), not a new format:

| VA | source |
|---|---|
| `0x60aa70` | real retail bytes, 9 decoded blocks |
| `0x604750` | real retail bytes, 1 block |
| `0x451a30` | **stub**: deterministic registry lookup. Compares the key with `[0x705000]` and returns `[0x705004]` (0 = miss). Replaces the lock/hash-map body, which needs the engine's BSMemory state. |
| `0x451a60` | **stub**: caller `mov eax,[esp+4]; mov ecx,[eax+0x28]; call 0x60aa70; ret`, mirroring the real sites at `0x5fbb95`/`0x5fc860`. |

The test writes the blob to `%TEMP%\menu_repro_<pid>.irc`, binds it with
`n3ds_lift_set_container_path` + `n3ds_lift_bind`, and invokes `0x451a60` via
`n3ds_lift_invoke`. The container tier is forced (the only AOT entry is a dummy
at `0x10000`), so execution is the predecoded IR path.

## 4. What the test covers - and what it does not

Covered (all asserted, not "did not crash"):

* **A - valid path.** `this` mapped, `+8` -> string, registry hit, tag `'G'`
  and `'<'` -> returns the record; tag `'X'` -> NULL; key miss -> NULL.
* **A' - `+8 == 0`.** Falls back to `+0xc` and still returns the record. This
  is not a fault and is asserted as such.
* **B - the reported crash.** `[record+0x28] = 0x62783041` ->
  `N3DS_INVOKE_TRAP`, stderr contains `addr=62783049`, `last_call=0060aa70`,
  `irc_fn=0060aa70`, and `n3ds_lift_fault()` is
  `Guest fault at 62783049: unmapped guest memory`.
* **C - previous crash class.** `[record+0x28] = 0` -> fault `addr=00000008`,
  same `last_call=0060aa70`. Honest correction: the code's fault address is
  `this+8` with `this == 0`; a *non-NULL* `this` whose `+8` is 0 is not a
  fault (scenario A').
* After both traps a valid invocation still succeeds and container stats show
  `execs >= 8`, `redecodes == 0` - pins recovered, no re-decode, no cache
  corruption.

Not covered, deliberately:

* The BSMemory multi-heap loop (`0xa48d60..0xa48f11`). The harness starts
  *after* the allocator; it cannot reproduce an allocator bug by itself.
* The real hash-map body `0x451a30` and the lock at `0x12e59e4` (the lookup
  is stubbed to a key/record pair).
* Who writes `item+0x28` in the menu path, and the 12-byte container walk.
* Indirect/vtable calls into `0x60aa70` (only the three direct call sites were
  scanned for xrefs).
* The AOT tier, ARM11 execution, and the device runtime. This is a host test
  of the container/IR tier semantics.
* Whether `0x62783041` is corruption or an uncommitted-but-real allocation;
  the harness takes it as an input value.

## 5. Files

| file | role |
|---|---|
| `tests/menu_crash_repro.c` | host test, registered as `menu_crash_repro` |
| `tests/menu_repro_irc.h` | generated 944-byte mini container (do not edit) |
| `tools/menu_repro_pack.py` | regenerates the container/header from the retail image |
| `tools/menu_repro.ps1` | one-command driver (`-Regen`, `-Full`) |
| `tools/run_host_tests.ps1` | `Add-Test "menu_crash_repro"` |

`menu_repro.ps1` creates a `staged/` junction to the main tree if the worktree
does not have the generated lift runtime yet (host tests need `staged/fpu.c`).
