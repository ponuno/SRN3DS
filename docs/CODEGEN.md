# CODEGEN — measured lifter code-size work

Status: **implemented and measured** (2026-11-24). Every number below was
produced in this workspace by `tools/codegen_bench.ps1`; nothing is a projection
unless labelled as one. ARM `.text` is the binding constraint on the port.

## 0. Correction to the first report

The first measurement reported selective spill/reload as **−10.0 %**. That
number came from a call summary that ignored what a callee's own callees read:
it was **unsound**. A function whose callee needs a register the function never
touches must still pass that register through, and the first version did not
require its caller to keep it live. The sound version (call-graph propagation +
per-instruction liveness for calls inside blocks) measures **−3.8 %**.

The larger win came from a different lowering: keeping the guest GPRs in
`CPU.r[]` and letting GCC's register allocator do liveness (**−13.1 %**). That
mode is sound by construction because `CPU.r` *is* the machine state.

## 1. The fixed benchmark

`tools/codegen_bench.ps1` slices a fixed branch of the real `TESV.exe`, lifts
the same slice once per variant, instruments it like the diagnostic builds, and
reports ARM `.text` from `arm-none-eabi-size`.

```powershell
# default: eager, lazy, selective, global on 2000 functions
powershell -File tools/codegen_bench.ps1

# linked check (tools/build.ps1 -Isolated cg; never touches the shared
# Skyrim_N3DS_tesv artifacts)
powershell -File tools/codegen_bench.ps1 -Measure link -Variant "global=LIFT_LAZY_FLAGS=1;LIFT_SPILL=1;LIFT_GLOBALREGS=1"

# differential host test + closure regression
powershell -File tools/codegen_bench.ps1 -Diff -Regression
```

Slice command (cached under `E:\Skyrim_N3DS_build\cgbench\slice`):

```
slice_pe.py E:\skyr\TESV.exe <functions.json> --root entry \
    --max-functions 2000 --max-insns 400000 --out <work>\slice --emit --allow-open
```

Fixed inputs for every object row: **2,000 functions, 511,020 x86 bytes,
151,687 x86 instructions, 33,451 basic blocks** (4.5 instructions/block), `-O2`,
devkitARM 15.2.0, instrumentation included.

Object mode compiles only the generated `.c` files with `arm-none-eabi-gcc`
under `E:\Skyrim_N3DS_build\cgbench\*`, so a benchmark is independent of
unrelated edits in `source/`. Linked mode confirms the ratio against a real
`.elf` (where the fixed runtime `.text` dilutes the percentage).

## 2. Results (generated-object `.text`, 151,687 x86 instructions)

| Variant | Env | ARM `.text` (B) | B/insn | vs eager |
|---|---|---:|---:|---:|
| eager (pre-change) | `LIFT_LAZY_FLAGS=0;LIFT_SPILL=0;LIFT_GLOBALREGS=0` | 4,403,351 | 29.03 | — |
| lazy flags | `LIFT_LAZY_FLAGS=1;LIFT_SPILL=0;LIFT_GLOBALREGS=0` | 4,392,869 | 28.96 | −0.2 % |
| sound selective spill | `LIFT_LAZY_FLAGS=1;LIFT_SPILL=1;LIFT_GLOBALREGS=0` | 4,235,089 | 27.92 | −3.8 % |
| **global registers** | `LIFT_LAZY_FLAGS=1;LIFT_SPILL=1;LIFT_GLOBALREGS=1` | **3,825,444** | **25.22** | **−13.1 %** |

Linked `.elf` `.text` (same runtime/source tree, `-O2`, `-Isolated cg`):

| Variant | ARM `.text` (B) | B/insn |
|---|---:|---:|
| eager | 4,814,656 | 31.74 |
| global | 4,238,624 | 27.94 |
| delta | **−576,032 (−12.0 %)** | |

Negative result: `-Os` measured 4,031,072 B on the old selective variant, worse
than `-O2`. Keep `-O2`. Instrumentation (`n3ds_pilot_tick` after every block)
costs 146,079 B (3.4 %) on this slice.

### 9,000-function linked re-measurement

Same harness, `-MaxFunctions 9000 -MaxInsns 1200000`, `-Measure link`,
`-Isolated cg`:

| Slice | x86 insns | ARM `.text` | B/insn |
|---|---:|---:|---:|
| recorded earlier (`docs/MEMORY_BUDGET.md` §3) | 924,440 | 26,112,064 (24.9 MiB) | 28.2 |
| this work, global registers | 910,984 | **23,095,008 (22.0 MiB)** | **25.35** |

That is **−11.6 %** bytes and **−10.1 %** bytes/instruction on a real 9,000
function `.elf` (the instruction totals differ slightly because of the slice
budget, hence both figures).

## 3. Lazy x86 flags

`staged/lift.py` computes, per function, whether each flag writer's output is
read before being overwritten. Only writers whose flags reach a consumer
(`jcc`, `setcc`, `cmovcc`, `adc`/`sbb`, `rcl`/`rcr`, `lahf`, `pushfd`,
`cmps`/`scas`, x87 `fcmov`/`fcomi`) materialise `cf/pf/zf/sf/of`.

- `flag_effect()` returns (reads, writes) bitmasks. Anything not modelled (and
  not in `NO_FLAGS`) falls back to **reads all + writes all**, so an unmodelled
  instruction can only cost size.
- Partial writers are modelled exactly: `inc`/`dec` preserve `cf`, `mul`/`imul`
  write `cf/of`, `bt*`/`clc/stc` write `cf`, `comiss` writes
  `cf/pf/zf` and clears `of/sf`, `bsf/bsr` write `zf`.
- **`cmc` reads and writes `cf`** (`FLAG_EFFECT["cmc"] = (F_CF, F_CF)`). An
  earlier version modelled it as write-only, so `clc; stc; cmc` dropped the
  `stc` and complemented the wrong incoming carry. The differential corpus has
  a permanent `clc; stc; cmc` case; the IR equivalence test also checks it.
- Variable-count shifts may leave all flags unchanged (count 0), so they are
  treated as reading every flag. This is what makes `shl reg, cl; jc` correct.

**Measured: −0.2 %.** GCC already eliminates dead stores to the local `Flags f`
at `-O2`; the change removes C statements, not machine instructions. It is kept
for explicitness and compile time, not as the size lever the earlier estimate
assumed.

## 4. Selective spill/reload (sound version)

Each lifted function derives, from its x86 body:

- `written`: every GPR it may assign (explicit operands; implicit writes from
  `mul`/`div`/`push`/`pop`/`call`/`ret`/`cpuid`/string ops/...; plus `esp`).
- `load`: every GPR whose incoming value the body can observe — read before
  definition (must-defined dataflow over the block CFG), conditionally written
  (`cmovcc`, `cmpxchg`, `cmpxchg8b`, `bsf`/`bsr`, `rep` string ops), `esp`, and
  **the `load` set of every callee minus registers definitely defined before
  that call**.

The callee contribution is solved on the call graph (`compute_summaries()`):
values only grow and a worklist propagates them, so a function that merely
passes a callee-saved register through to another function still requires its
caller to supply it.

Calls inside a basic block are normal in this slice (the discovery blocks split
on branches, not calls). For each call the reload set is computed by a backward
scan from the end of *that block*, and a call is modelled for liveness as
"reads the callee's needs, writes everything" — every register live after it is
reloaded, and the rest are dead.

Target classification:

- direct call to a lifted function → its `load` set;
- `call [iat]` / import thunk → `HOST_CALL_READS = {esp}`. The shims in
  `source/win32_*.c` read arguments from `c->r[4]` and write `c->r[0]`; none
  reads `ecx/edx/ebx/esi/edi/ebp` (verified by grep and by
  `tools/check_shim_bases.py`);
- indirect target, host override, or a direct target outside the slice → **all
  registers** (conservative fallback).

The epilogue stores `written`; `c->r` keeps every register the function did not
touch. A register written by the function is read by its own epilogue, so
liveness keeps it live across earlier calls and it is reloaded when needed.

Measured masks: spill averages **5.65** registers (was 8), reload **3.54**
(was 8) over 14,968/18,517 sites. **Measured: −3.8 %** (4,403,351 →
4,235,089 B). Superseded by §5.

## 5. Global register file (the big win)

`LIFT_GLOBALREGS=1` (default) removes the C locals for `eax..edi` and emits

```c
#define eax c->r[0]
#define ecx c->r[1]
...          /* same for edx, ebx, esp, ebp, esi, edi */
```

inside each generated function. `CPU.r` is now the machine state itself:

- no prologue/epilogue loads-stores of the eight GPRs;
- no hand-written spill/reload: the existing `SPILL*`/`RELOAD*` statements
  become self-assignments and are eliminated by GCC;
- calls are ordinary C calls; GCC keeps values in ARM registers and reloads
  exactly what is live across the call.

This is **sound by construction** — there is no summary to get wrong, and guest
semantics (including a callee clobbering a caller-saved register) are modelled
directly. It also makes the lift fast again: summaries/liveness are skipped.

**Measured: −13.1 %** (4,403,351 → 3,825,444 B, 29.03 → 25.22 B/insn);
linked −12.0 %.

Fallback: `LIFT_GLOBALREGS=0` restores C locals; combined with `LIFT_SPILL=0`
and `LIFT_LAZY_FLAGS=0` it reproduces the original generated C exactly (same
4,403,351 B).

## 6. Guest-memory fast path — measured, and why it is a regression

Every generated guest access is `M32(addr)` → `n3ds_guest_ptr(addr,4)` → a
linear region scan (`source/guest.c:guest_ptr`). Two experiments on the same
slice:

1. **Unchecked direct dereference** (`base + addr`, compile-only; ignores the
   region model). Upper bound of any perfect window:

   | Base | `.text` | vs same variant |
   |---|---:|---:|
   | selective | 3,562,636 (23.48 B/insn) | −15.9 % |
   | global | 3,161,520 (20.84 B/insn) | −17.4 % |

2. **The specified fast path**, implemented and measured: a contiguous-window
   accessor with a bounds check and a slow-path call on a miss
   (`n3ds_mem()` → `n3ds_guest_window + off`, else `n3ds_guest_ptr`), inlined per
   access. Result on the global variant: **6,002,256 B (39.57 B/insn), +57 %**.
   The experiment was reverted.

The per-access check (compute offset, two compares, branch, host pointer, then
the load/store) costs far more than the `bl` to the region scan it replaces.
This confirms the earlier negative result in `docs/MEMORY_BUDGET.md` §5.2 (an
inline check made one function 2.35× larger).

The only way to get the −16 % is to remove the check, which requires every
guest address to be backed by committed host memory. The 3DS has no MMU, the
guest is a list of separately `calloc`'d regions, and an unchecked
`window + addr` would read outside the allocation instead of raising the
runtime's `unmapped guest memory` fault. That violates the "identical trap
behaviour" rule, so the fast path is **not implemented**.

A narrower, self-contained variant remains possible: validate a function's whole
static stack frame once at entry and use raw offsets for `esp`/`ebp` accesses
when it succeeds (slow path otherwise). It needs a per-function frame-extent
analysis and a fallback branch per access; given the measured cost of inline
checks, it is unlikely to be a size win and was not attempted.

## 7. Correctness evidence

### Differential machine-state test — `tools/codegen_diff.py`

Assembles a self-authored i386 corpus (no game data), wraps it in a PE, lifts it
four ways and builds four host binaries:

| Mode | Flags |
|---|---|
| `eager` | `LAZY=0`, `SPILL=0`, `GLOBALREGS=0` — pre-change behaviour |
| `lazy` | `LAZY=1`, `SPILL=0`, `GLOBALREGS=0` |
| `selective` | `LAZY=1`, `SPILL=1`, `GLOBALREGS=0` |
| `global` | `LAZY=1`, `SPILL=1`, `GLOBALREGS=1` |

It runs all 16 functions on 16 deterministic input vectors and compares all
eight GPRs, `top`, `df`, `eflags_hi`, and the EFLAGS saved by each function via
`pushfd`/`lahf` into its output buffer.

Coverage: `add/sub/adc/sbb/cmp`, `and/or/xor/test`, `inc/dec/neg`, immediate and
`cl` shifts/rotates (`rcl/rcr`), `mul/imul`, `bt/bts/btr/btc`, `clc/stc/cmc`,
`setcc`/`cmovcc`, all `jcc` directions, `sahf/lahf/pushfd/popfd`,
`comiss/ucomiss/comisd`, `bsf/bsr`, `loop/jecxz`, `repe cmpsb`, 8/16-bit
partial-register writes, multi-block branches with backward edges, and direct
calls into generated functions (including a bare register-only callee).

Result: **PASS — eager, lazy, selective and global agree on 256 state
snapshots.**

### Real closure regression — `tools/lift_regression.py`

```
OK: slice_functions.json: 25360 functions in 108.6s (lifted 25360 rejected 0)
```

slice: `E:\Skyrim_N3DS_build\analysis\growth11\iter01\slice_functions.json`.
This is the check that caught the original `order`/`decoded` key mismatch
(tuple block keys vs integer starts). `codegen_bench.ps1 -Regression` runs it.

### Standard project gates

- `tools/run_host_tests.ps1 -OutDir E:\Skyrim_N3DS_build\host` — 23 passed,
  0 failed (includes `tools/check_shim_bases.py`).
- `tools/build.ps1 -Mode lift` — succeeds.

## 8. Fallbacks

```
LIFT_LAZY_FLAGS=0     # eager flag materialisation
LIFT_SPILL=0          # full eight-register spill/reload around calls
LIFT_GLOBALREGS=0     # C locals instead of CPU.r[]
```

`LAZY=0 + SPILL=0 + GLOBALREGS=0` reproduces the original output exactly
(4,403,351 B on the fixed slice).

## 9. Projection and the 54 MiB target

The closed startup closure is 2,900,299 x86 instructions (from
`docs/HYBRID_EXECUTION.md`). Applying the measured 25.22 B/insn:

| Estimate | `.text` |
|---|---:|
| previous (28.2 B/insn) | ~78 MiB |
| global registers now (25.35 B/insn, 9k measurement) | **~70 MiB** |
| global + unchecked memory window (20.84 B/insn; not trap-equivalent) | ~58 MiB |
| ...and no per-block instrumentation (−3.4 %) | ~56 MiB |

**The ≤54 MiB target is not reachable with the levers available to me.** The
remaining gap needs the contiguous guest window (a `source/` change) *and*
another lowering change. The most promising one is the block-level AOT design
already proposed in `docs/HYBRID_EXECUTION.md` §4.4, or simply moving the cold
majority of the closure to the IR interpreter (§10), which is 3× denser than
AOT text.

## 10. Hybrid IR instruction shape (design note, not implemented)

Measured instruction shapes of the fixed slice:

| Shape | Share |
|---|---:|
| instructions with a memory operand | 35.5 % |
| of those: no displacement / disp8 / disp32 | 13.5 % / 44.8 % / 41.7 % |
| register-only (no memory, no immediate) | 30.6 % |
| immediate operands: fit in 8 bits / need 32 | 29.7 % / 70.3 % |
| operands per instruction: 0 / 1 / 2 / 3 | 1.6 % / 45.5 % / 52.9 % / 0.05 % |
| basic-block length | 4.5 instructions |

Proposed encoding — one 32-bit **core word**, always present:

```
 bits  0..7    opcode (ALU/branch/mov/call/flag ops)
 bits  8..11   dst: register index, condition code, or flag mask
 bits 12..15   src: register index
 bits 16..17   operand size (8/16/32/64)
 bit   18      memory operand present
 bit   19      32-bit immediate follows
 bits 20..31   signed 12-bit imm/disp (covers imm8 and disp8)
```

Optional **address word** when bit 18 is set:

```
 bits  0..3    base register
 bits  4..7    index register
 bits  8..9    scale
 bits 10..13   access size + width
 bits 14..31   reserved
```

plus a 32-bit displacement word when it does not fit the 12-bit field, and a
32-bit immediate word when bit 19 is set. A 16-bit short form
(`op:6 dst:3 src:3 size:2 flags:2`) covers the register-only subset.

Cost on the measured mix:

| Component | Bytes |
|---|---:|
| core word | 4.00 |
| address word, 35.5 % of instructions | 1.42 |
| disp32 word, 41.7 % of those | 0.59 |
| imm32 word, 0.386 immediates/insn × 70.3 % | 1.09 |
| block header (8 B / 4.5 instructions) | 1.78 |
| short-form saving | ≈ −0.5 |
| **total** | **≈ 8.4 B/insn** |

That is 3.0× denser than the current AOT output (25.35 B/insn): an 8 MiB
decoded-IR cache holds ~1.0 M guest instructions plus headers, enough for the
measured hot set (99 functions at startup; top 32 cover 87 % of calls). The IR
is the right home for the part of the closure that does not fit AOT, and it is
what makes the 178 MiB device budget workable.

## 9. Differential regression: `cmc` liveness

The AOT/IR equivalence corpus exposed a real flag-liveness bug, not an IR
problem: `cmc` reads and writes CF, but `FLAG_EFFECT["cmc"]` was `(0, F_CF)`.
Backward liveness therefore deleted a preceding `stc`, and generated code
complemented the wrong incoming CF. The effect is now `(F_CF, F_CF)`.

Permanent reproducer: `clc; stc; cmc`, with branches observing CF before and
after a second `cmc`. Before the fix, AOT returned the wrong CF and branch-side
register results. After the fix, eager, lazy, selective-spill and global-register
variants agree on **272 state snapshots (17 functions × 16 inputs)** in
`tools/codegen_diff.py`. A regression lift of 25,360 real functions completed
with 0 rejected functions.
