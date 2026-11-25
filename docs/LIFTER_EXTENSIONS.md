# Lifter extensions: segment registers and loope/loopne

Scratch tree: `E:\Skyrim_N3DS_invalid_triage`. The live `staged/lift.py` was not
modified. The modified copy is `lift_v2.py` (SHA-256
`4297F3C94DE7554E43B0408FA2868DA4A058AE41A6978B07E5F71838426E3931`,
2,073 lines vs 2,023; +62 / -12 lines). The full diff is `lift_v2.diff`.

All runs used the real `E:\skyr\TESV.exe`, `invalid_functions.json` (the same
873-function subset as the first triage), jobs=1 (default, no `--jobs`), and
`MOD=triage`. No live tree, container, slice, or F: card was touched.

## 1. What changed and why

### 1.1 `pop sreg` (es/cs/ss/ds/fs/gs)

`POP ES`/`CS`/`SS`/`DS`/`FS`/`GS` previously reached `reg_set()` and raised
`Unsupported("reg es")` etc. The emitter now handles it before the generic pop:

```c
{ (void)M32(esp); esp += 4; }     /* 32-bit operand size */
{ (void)M16(esp); esp += 2; }     /* 0x66 prefix */
```

`pop_sreg()` selects 4 or 2 bytes from capstone's prefix list. The stack slot is
read (not just skipped) so an unmapped stack still faults exactly where the
guest would fault, then the value is discarded.

**Semantic decision.** Only the stack effect is modelled. The runtime has no
architectural selector state: `CPU` carries `r[8]`, `fs_base`, `df` and
`eflags_hi`, but no CS/DS/ES/SS/FS/GS selector or hidden base. `addr()` ignores
CS/DS/ES/SS overrides entirely and models only `fs_base` (FS) while rejecting
GS overrides.

**Safety.** For ES/CS/SS/DS this is safe under the flat-32-bit-Windows model:
the bases are fixed and ignored by every emitted memory access, and reads of
the selector return the fixed selectors below, so the popped value is not
observable. For FS/GS the stack effect is still correct, but a `pop fs`/`pop gs`
that actually changes the selector would silently not rebase FS/GS. This is an
approximation, not general x86 emulation. The new set contains exactly one
`pop fs` and no `pop gs`; both were data/likely-dead sequences. If strictness is
required, make `pop fs/gs` emit `rt_unimpl` after the stack adjustment.

### 1.2 `mov sreg, r/m16` (0x8e) and `mov r/m16, sreg` (0x8c)

The read direction was already modelled before this change: `reg_get()` returns

```c
((uint16_t)0x2B)  /* es, ds, ss, gs */
((uint16_t)0x23)  /* cs */
((uint16_t)0x53)  /* fs */
```

These are the same constants as the IR decoder's `sel[]` table in
`source/ir_decode.c` (`{0x2b,0x23,0x2b,0x2b,0x53,0x2b,0,0}`), so this is not a
new fake value: it is the existing flat-Win32 selector model shared by both
tiers. A real Win32 process gets these selectors from the loader; code that
loads an arbitrary different selector would #GP, not observe it. Caveat: a write
that is dropped (below) followed by a read returns the constant, not the dropped
value. That is the flat-model approximation.

The write direction (0x8e) was previously rejected for every destination. It is
now:

| Destination | Emitted | Why |
|---|---|---|
| `es`, `ds`, `ss` | `(void)TL16(src);` (memory) or `(void)((uint16_t)reg);` | selector not observable; base ignored by `addr()`; the read is kept so a memory operand still faults |
| `fs`, `gs` | `SPILL(); rt_unimpl(c, addr, "mov fs"/"mov gs");` | loading FS/GS changes the hidden base used for `fs:`/`gs:`; a 16-bit selector is not enough to derive it |
| `cs` | `SPILL(); rt_unimpl(c, addr, "mov cs");` | `MOV CS,r/m16` is #UD on 386+; a real CPU faults here |

So a read is modelled (fixed selector constants); an FS/GS/CS write is an
**explicit, named runtime fault** at the exact instruction address, not a silent
success. Functions containing those writes are still accepted by the lifter and
will run until the faulting instruction. 17 `mov cs`, 2 `mov fs` and 2 `mov gs`
sites are in the newly accepted output; the addresses are listed in
`liftable_aot_v2.txt`.

### 1.3 `loope` / `loopne`

`loope` and `loopne` were in `JUNK`. They are removed from `JUNK` and emitted
like the existing `loop`:

```c
if (--ecx && f.zf)  ...   /* loope  */
if (--ecx && !f.zf) ...   /* loopne */
```

The decrement is `uint32_t`, so ECX=0 wraps to 0xFFFFFFFF and the branch is
taken when ZF allows, which matches x86 `LOOP`/`LOOPE`/`LOOPNE`.

**Lazy flags.** `FLAG_EFFECT` now maps `loope`/`loopne` to `(F_ALL, 0)`, the
same conservative flag read used for `jcc`. Without this the backward
flag-liveness pass would not know ZF is read at the loop terminator and could
drop the materialisation of ZF. `loop` and `jecxz` stay `(0, 0)`.

**Terminator predicates.** capstone reports `loop`/`loope`/`loopne` with
`X86_GRP_BRANCH_RELATIVE`, not `X86_GRP_JUMP` (only `jecxz` gets `jump`), so
each predicate must name them explicitly. Added a shared
`LOOP_TERMS = ("loop", "loope", "loopne", "jecxz")` and used it in all of:

1. `FLAG_EFFECT` (reads all flags);
2. `_is_term()` (module-level summary analysis);
3. `plain_succs()` (successor blocks for summaries);
4. `analyze_function().hard_term()` (mid-block branch detection);
5. `Lifter._block_succs()` (lazy-flag CFG);
6. `Lifter.lift()` (mid-block terminator guard);
7. `Lifter.is_term()` (emission boundary/fall-through);
8. `Lifter.lift_terminator()` (the emitter itself).

That is every place the old code special-cased `("loop", "jecxz")` or tested
for a terminator.

## 2. Proof

### 2.1 Run

```
PYTHONPATH=staged python lift_v2.py E:\skyr\TESV.exe ^
    invalid_functions.json lift_out_v2 triage 400000
```

Result: **605 lifted, 268 rejected** out of 873 (previously 418/455).

| | v1 `staged/lift.py` | v2 `lift_v2.py` |
|---|---:|---:|
| accepted | 418 | **605** |
| rejected | 455 | **268** |
| new accepts | — | **187** |

New accepts by the original measured root cause:

| Original root cause | Newly accepted |
|---|---:|
| segment register push/pop/mov | 180 |
| `loopne` JUNK | 6 |
| segment override | 1 |
| **total** | **187** |

Newly accepted functions contain (instruction occurrences):

| Construct | Occurrences |
|---|---:|
| `pop es` | 720 |
| `pop ss` | 151 |
| `pop ds` | 59 |
| `pop fs` | 1 |
| `mov es/ds/ss, r/m16` (discarded read) | 6 |
| `mov cs, ...` (explicit fault) | 17 |
| `mov fs, ...` (explicit fault) | 2 |
| `mov gs, ...` (explicit fault) | 2 |
| `loope` | 6 |
| `loopne` | 6 |

The large `pop es` count is because these are instruction occurrences across
each function's discovery ranges, and several of these functions have
overlapping ranges; the function count is 165 with at least one `pop sreg`.

### 2.2 Control: previously accepted output is byte-identical

`compare_v2.py` extracts every `void f_X(CPU *c) { ... }` body from
`lift_out/triage_*.c` and `lift_out_v2/triage_*.c` and compares the text:

* 418 / 418 baseline bodies are **byte-identical**;
* 0 baseline functions were dropped;
* 0 baseline functions are now rejected;
* 187 new bodies added.

The declaration header `funcs_triage.h`, the function table and the bucket file
layout necessarily grow/repartition (the accepted address span is wider), so
whole-file hashes differ by design; per-function output does not.

### 2.3 Syntax

All 24 generated bucket files plus `triage_table.c` pass:

```
clang -fsyntax-only -w -std=gnu11 -I staged -I source lift_out_v2/triage_*.c
```

0 errors, 3.58 s.

### 2.4 Executable semantics test

`build_semantics.py` extracts the **verbatim emitted statements** from the
generated files, compiles them with `lift_prelude.h` against a small guest
buffer and a `rt_unimpl` that longjmps, and runs a truth table
(`semantics_test.c`):

* `pop sreg`: ESP advances by exactly 4, the slot is read, and an unmapped
  stack faults (the read is preserved).
* `loopne`: (ECX,ZF) = (2,0) taken, (2,1) not, (1,0) not, (1,1) not,
  (0,0) taken (wrap), (0,1) not; ECX decremented in every case.
* `loope`: (2,1) taken, (2,0) not, (1,1) not, (0,1) taken, (0,0) not.
* `mov es/ds/ss, mem`: a mapped 16-bit read succeeds; an unmapped read faults.
* `mov cs` / `mov fs` / `mov gs`: longjmp with exactly `"mov cs"`, `"mov fs"`,
  `"mov gs"` at the encoded guest address.

Result: `SEMANTICS: all checks passed` (compile 0.21 s, run 0.04 s).

This is a unit test of the emitted constructs, not a run of the 605 functions
against the game.

## 3. Remaining rejects and why

268 still rejected. By the v2 rejection reason:

| v2 reason | Count |
|---|---:|
| `junk les` | 62 |
| `junk enter` | 38 |
| `junk int1` | 33 |
| BCD: `aas` 24, `daa` 19, `das` 16, `aam` 15, `aaa` 13, `aad` 13 | 100 |
| `junk fstpnce` | 11 |
| `junk lds` | 10 |
| `addr16` | 8 |
| `segment` (GS override still rejected by `addr()`) | 5 |
| `df /6` | 1 |
| **total** | **268** |

By original root cause, 20 of the 200 segment-register functions and 4 of the
10 `loopne` functions still fail, because after removing the first blocker a
second unsupported instruction surfaced (LES/LDS, ENTER, BCD, INT1, GS
override). The 5 remaining `segment` rejects are GS-prefixed memory operands;
`addr()` only models FS and raises `Unsupported("segment")` for GS.

The old decoder work list is otherwise unchanged: 86 BCD, 69 LES/LDS, 38 ENTER,
32 INT1, 12 x87, 5 address-size, and 5 GS-prefix rejects (2 were already in the
override group; 3 surfaced behind the segment fixes).

## 4. Runtime semantics caveats per construct

| Construct | Caveat |
|---|---|
| `pop es/ss/ds` | Selector discarded; stack effect and slot fault are exact. Safe because those bases are ignored everywhere. |
| `pop fs` | Stack effect exact, but a selector change would not rebase FS. One occurrence; approximate. |
| `pop gs` | Same as FS; none in the new set. |
| `mov es/ds/ss, r/m16` | Selector write dropped; memory read kept for faults. Safe in the flat model; a write-then-read-back sequence would see the fixed constant, not the written value. |
| `mov fs/gs, r/m16` | Explicit `rt_unimpl` fault. Does not read memory before faulting (differs from a real CPU only if the memory operand itself is unmapped). |
| `mov cs, r/m16` | Explicit `rt_unimpl` fault, matching #UD. |
| `mov r/m16, sreg` | Already modelled with fixed selectors; same constants as `ir_decode.c`. |
| `loope`/`loopne` | ECX decrement and ZF condition match. ZF is materialised via `FLAG_EFFECT`. **Control-flow caveat:** `goto()` only knows emitted block starts; 11 of the 12 new loop instructions target an address that discovery did not mark as a block start, so they become a tail `rt_call`/`f_target(c); return;` exactly like the pre-existing `loop` emitter. Only `f_00F7E182` loops to its own label. The 7 affected functions are listed in `liftable_aot_v2.txt`; adding the loop targets as discovery leaders is the correct fix before relying on these particular bodies. |

The 15 functions in `liftable_aot_v2.txt` that contain a named `rt_unimpl`
(13 new: 7 `mov cs`, 2 `mov fs`, 2 `mov gs`, plus 2 pre-existing `xrelease
xchg`/`wbinvd`) are accepted because the fault is explicit and named. They are
not silently wrong: they fault at the exact guest address.

## 5. Wall time

| Stage | Wall |
|---|---:|
| `py_compile lift_v2.py` | 0.1 s |
| `lift_v2.py` jobs=1 over 873 functions | **15.56 s** |
| `clang -fsyntax-only` 25 files | 3.58 s |
| byte-for-byte control comparison | 0.24 s |
| instruction audit (capstone over 187 functions) | 6.57 s |
| semantics harness compile + run | 0.25 s |
| output inspection + list generation | 0.43 s |
| **main chain** | **~26.2 s** |

## 6. Limitations

* Acceptance is `lift.py` accept + `clang -fsyntax-only`, plus the focused
  semantics unit test above. The 605 functions were not linked into the 3DS
  runtime or executed against TESV.
* The input is the prescribed 873-function `functions.json`; calls outside it
  are "unknown" for liveness. The 418 control is byte-identical, so the subset
  has no observable effect on the previously accepted set.
* `loope`/`loopne` bodies with out-of-function targets inherit the existing
  `loop` tail-call fallback; that is a discovery/CFG issue, not an emitter bug.
* The FS/GS selector approximation is a flat-model decision, not full x86
  segment emulation.
