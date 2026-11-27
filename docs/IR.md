# IR — compact decoded x86 for the cold tier

Status: **implemented, measured and differentially tested** (2026-11-24).
This supersedes the design note in `docs/CODEGEN.md` §10 and the sketch in
`docs/HYBRID_EXECUTION.md` §5. Every number below was produced in this
workspace by the tools named next to it.

v2 adds x87 and the high-impact SSE/SSE2 subset; the previous integer-only
version left 4,555 functions (3.89 M x86 bytes) without an interpreter tier.

| File | Role |
|---|---|
| `source/ir_core.h` | format definition, opcode list, public API |
| `source/ir_core.c` | exact-entry function table (`n3ds_ir_bind/has/end`) |
| `source/ir_decode.c` | x86-32 decoder → IR, bounded decode cache |
| `source/ir_exec.c` | interpreter (integer, x87, SSE/SSE2, MMX rejected) |
| `source/n3ds_lift_runtime.c/h` | tier dispatch, invoke, weak IR-table binding |
| `source/ir_bench_main.c` | ARM11/Azahar AOT-vs-IR microbenchmark |
| `tools/ir_select.py` | profile → AOT list + manifest + IR table |
| `tools/ir_measure.py` | real decoder measurement, histograms, ARM size |
| `tools/ir_validate.py` | partition/manifest validator |
| `tools/ir_make_fixture.py` | builds the equivalence corpus and AOT fixture |
| `tests/ir_equiv.c` | differential AOT/IR test |
| `tests/ir_corpus.h`, `tests/ir_aot_fixture.c` | generated corpus / AOT tier |
| `tests/ir_partition_test.py` | emitted-table/manifest validation test |

## 1. Measured size

### The fixed 2,000-function benchmark slice

Same slice and fixed inputs as `tools/codegen_bench.ps1` (2,000 functions,
511,020 x86 bytes, 151,687 listed instructions):

```
python tools/ir_measure.py --unique --arm-size
  functions: 2000 (2000 decode)
  union x86 bytes: 304,492
  unique blocks: 14,378 blocks, 64,884 instructions,
                 555,728 IR bytes (8.56 B/insn), 0 undecodable blocks
  per-function pass (duplicated discovery ranges): 119,602 insns, 1,051,424 B
  ARM11 interpreter .text (-O2): 55,338 B (0.05 MiB)
```

The per-function pass over-counts because `functions.json` lists shared basic
blocks once per containing function (708,349 ranges for 25,360 functions on the
full slice). **The unique-block pass is the honest size**; the per-function
pass is kept only for per-function coverage and the fail list.

### The real 25,360-function slice (`growth12/iter01`)

```
python tools/ir_measure.py --exe E:\\skyr\\TESV.exe \
  --functions E:\\Skyrim_N3DS_build\\analysis\\growth12\\iter01\\slice_functions.json \
  --fail-list ... --unique --histogram
  functions: 25360 (25350 decode)
  union x86 bytes: 2,881,478
  unique blocks: 166,694 blocks, 794,332 instructions,
                 6,557,712 IR bytes (8.26 B/insn), 4 undecodable blocks
  functions with an unsupported opcode: 10; their union bytes: 10,597 (0.37%)
```

The 10 remaining functions are **not** opcode gaps: they hit
`IR: block has no terminator at <addr>` at three shared cold-block boundaries
(`0x489e9a`, `0x5fdc06`, `0xb0d680`) where the discovery range ends but the
instruction stream continues without a terminator inside `max(hi)`. Forcing
them AOT costs 34,514 x86 bytes / 10,078 instructions / **0.24 MiB** of AOT
text. That is the entire remainder.

Coverage by both measurements (full slice):

| Set | Functions | Instructions | Union x86 bytes | Projected |
|---|---:|---:|---:|---:|
| Supported by IR | 25,350 | ≈784k | 2,870,881 (99.63%) | ≈6.18 MiB IR |
| Forced AOT (terminator) | 10 | 10,078 (summed) | 10,597 (0.37%) | 0.24 MiB AOT |
| Total | 25,360 | 794,332 unique | 2,881,478 | 6.42 MiB |

The 6.24 MiB unique IR pass is slightly lower than 6.18 + 0.24 because the
forced functions' decodable prefixes are already in the IR figure. The 22.8
MiB closure projection is the density applied to the full 2.9 M-instruction
closed closure, not to this slice.

### Projection (closure = 2,900,299 x86 instructions)

AOT density is the measured 25.35 B/insn (`docs/CODEGEN.md`, 9,000-function
`.elf`); IR density is the measured 8.26 B/insn.

| Mix | AOT | IR | Total |
|---|---:|---:|---:|
| 100 % IR | 0.0 MiB | 22.8 MiB | **22.8 MiB** |
| 20 % AOT / 80 % IR | 14.0 MiB | 18.3 MiB | **32.3 MiB** |
| 50 % AOT / 50 % IR | 35.1 MiB | 11.4 MiB | 46.5 MiB |
| 100 % AOT | 70.1 MiB | 0.0 MiB | 70.1 MiB |

The device budget is ≤ ~54 MiB of text next to the 85 MiB guest arena
(`docs/MEMORY_BUDGET.md`). 100 % IR and 20 % AOT both meet the 30–35 MiB
target; the forced-AOT remainder adds 0.24 MiB. The 8 MiB decode cache is not
part of these text figures.

## 2. IR format

Every instruction starts with one 32-bit **core word**; optional words follow
only when needed. All words are little-endian `uint32_t`.

```
core word
  bits  0..7   opcode (IrOp)
  bits  8..10  dst register index (or string kind)
  bit   11     dst is a memory operand
  bits 12..14  src register index
  bit   15     src is a memory operand
  bits 16..17  size: 0 = 8, 1 = 16, 2 = 32
  bits 18..19  immediate: 0 none, 1 imm8 signed, 2 imm32 follows,
                          3 imm8 unsigned
  bits 20..21  segment: 0 none, 1 fs, 2 gs (gs rejected)
  bits 22..23  aux (shift count from cl, string rep kind)
  bits 24..31  imm8 payload

address word (iff dst or src is memory)
  bits  0..3   base, 15 = none       bits 4..7  index, 15 = none
  bits  8..9   scale                 bits 10..11 displacement kind
  bits 24..31  disp8 payload
disp32 word   (displacement kind 2)
imm32 word    (immediate kind 2, or the x87/SSE descriptor)
```

x87 and SSE instructions carry one descriptor word after the address words:

```
x87:  (primary_op << 8) | modrm            primary_op = 0xd8..0xdf
SSE:  (op2 << 24) | (prefix << 22) | (modrm << 8) | imm8
      prefix: 0 none, 1 0x66, 2 0xf2, 3 0xf3
```

This keeps x87 register forms at 8 B, x87 memory forms at 12 B, and the common
SSE register forms at 8 B. Branch displacements stay relative and are resolved
against the block end.

A basic block is an 8-byte header (`start`, `x86_len`, `count`, `term`)
followed by the stream. `x86_len` is also the fall-through address and the
relative-branch base. The terminator is always the last instruction.

`count` is one byte, so a block holds at most 255 x86 instructions. Compiler
output can be straight-line for longer than that: TESV's `0x429872` is a
333-instruction x87 run between a call and a `jmp`. When the cap is reached
before a guest terminator, `n3ds_ir_encode_block()` appends a synthetic
`IR_OP_JMP` with displacement 0 that targets `start + x86_len` and reports
`IrBlockInfo.split`. The remainder decodes as the next block, which is exactly
the CPU's fall-through. The packer uses `split` to continue a discovery span
past that block (it already did the same after a `call`), so one long span
becomes several record blocks with no format change. Regression:
`tests/ir_long_block_smoke.c`.

Measured word mix on the full slice (`growth12` unique pass): core 4.00,
address word 1.37, disp32 0.58, imm32 0.97, block header 1.34 → **8.26 B/insn**
measured. The 2,000-function slice measures 8.56 B/insn.

## 3. Decode cache and eviction

Decoding is on demand, one basic block at a time, straight from guest memory
through `n3ds_guest_ptr`. No decoded code is ever written to disk.

- Arena: one `calloc` buffer, default **1 MiB** (`N3DS_IR_DEFAULT_CACHE`),
  configurable with `n3ds_ir_cache_init(bytes)`.
- Index: open-addressed hash of the block start address.
- **Eviction:** when the remaining arena cannot hold a normal block (8 KiB
  reserve), the whole arena and index are flushed and decoding restarts. This
  is deterministic, O(1), and behaviour-neutral because decoding is a pure
  function of the guest bytes.
- Stats: `n3ds_ir_cache_stats()` reports blocks, bytes, hits, misses, flushes,
  evicted bytes.
- Not thread-safe; the runtime pins guest threads to one host thread.
  Self-modifying code is not invalidated, exactly like AOT code.

For an 8 MiB budget the measured block size is 37.1 B, so the cache holds
~226,000 blocks (≈986,000 instructions at 4.4 instructions/block). The 99
executed functions of the deep profile need **26 KiB** of IR (measured), so
startup does not depend on the cache budget.

## 4. Interpreter semantics

The interpreter uses the same machine state as AOT:

- **Registers** are `c->r[0..7]` directly; `getr/setr` implement 8-bit high
  bytes (`ah/ch/dh/bh`), 16-bit partial writes and 32-bit writes. This is the
  global-register AOT convention (`LIFT_GLOBALREGS=1`).
- **Flags** are a per-frame `Flags f = {0}`, exactly like a generated AOT
  function: a call preserves the caller's flags for both tiers and a callee
  starts with zeroed flags. `df`/`eflags_hi` live in `CPU`.
- **Arithmetic** calls the same `add8/16/32`, `adc`, `sbb`, `logic`,
  `inc/dec/neg`, shift/rotate, `shld/shrd`, `mul/imul/div/idiv` helpers from
  `staged/x86.h` as AOT code, so flag bits cannot drift.
- **x87** uses the same `fpush`, `fpop`, `fcompare`, `fround`, `fist*`,
  `f80_load/store`, `fxam`, `fprem`, `fpu_env` helpers and updates
  `c->top/c->st[]/c->c0..c3/c->fpu_cw` identically. The x87 stack, top,
  condition codes and `fnstsw` are compared bit-for-bit by the equivalence
  test.
- **SSE** uses `c->xmm[]`, `c->mxcsr` and the same lane semantics as the AOT
  lowering (`movss/movsd` upper-half rules, `unpck*`, `shuf*`, scalar vs
  packed arithmetic, `min/max` second-operand-on-NaN, `cmp*` predicate masks,
  `movmsk*`, `cvtt*`/`cvt*`, `ps*` packed integer ops).
- **Memory** is only `M8/M16/M32`/`n3ds_xmm_read` → `n3ds_guest_ptr`
  (checked). An unmapped access produces exactly
  `Guest fault at XXXXXXXX: unmapped guest memory`.
- **String ops** reproduce the AOT lowering, including the `rep movsd`
  `rt_movsd` fast path and `c->df` stepping. Both tiers call the same
  `n3ds_rep_movsd()`; see the host-pointer rule below.
- **`ret`** pops `4 + imm16` and returns from the interpreter frame; the popped
  value is ignored, exactly as the AOT `esp += 4 + n; return;` convention.
  Calls are C recursion; `ret` never dispatches on the stack value.
- **Tail `jmp`** outside the current function extent dispatches the target and
  returns from the frame, giving the callee a fresh flag set — the same thing
  AOT's `rt_call(c,target); return;` does.
- **`int3`/`int`/divide-by-zero** fault explicitly; `n3ds_lift_exit` uses the
  same `longjmp` frame as AOT.

### Host-pointer lifetime invariant (string copies)

A pointer returned by `guest_ptr()` / `n3ds_guest_ptr()` is valid only until
the next guest-memory call. Lazy commit can free host blocks while that
pointer is held:

- `slab_prepare()` bridges a multi-page range into one allocation and
  `page_drop_preserve()` frees the page allocations it replaces (pages of an
  old bridge outside the new range are reallocated at new addresses);
- at `m->limit`, `page_back()` calls `slab_evict_one()`, which flushes and
  frees another file-backed page.

`rep movsd` used to fetch `src` and `dst` host pointers and copy through them.
Fetching the second pointer could perform either operation, leaving the first
pointer dangling: ASan reports a heap-use-after-free, and an unsanitised run
may read a block the allocator already reused. This is not an x86 semantic;
the guest still sees a forward `DF=0` copy with dword overlap propagation.

Both tiers therefore call one helper, `n3ds_rep_movsd()` in
`source/n3ds_lift_runtime.c`; AOT `rt_movsd` and IR `rep_movsd` only keep their
watch tags. It never holds two raw pointers across a potential commit:

- a range wholly inside one `GuestRegion` uses `memmove()` (region data is
  allocated once and is not moved or freed while mapped);
- otherwise non-overlapping copies move page-bounded chunks through a stack
  buffer via `guest_read()` / `guest_write()`;
- `dst > src` overlap reads a dword then writes it, which is the exact forward
  `rep movsd` result; `ecx == 0` performs no access.

Plain `movs`/`rep movsb`/`rep movsw` elements are emitted as
`n3ds_movs8/16/32(dst, src)` by `tools/lift.py` (patched in
`tools/stage_upstream.py`): the source value is read before the destination
pointer is fetched, so a destination commit cannot invalidate the source.
`stos`/`lods` touch one address and keep the direct lowering.

Regression: `tests/rep_movs_smoke.c` forces eviction on every page commit
inside a file-backed slab copy (deterministic byte mismatch on the old code;
heap-use-after-free under `-Sanitize`), and `tests/ir_equiv.c` compares AOT and
IR on forward overlap and on a copy crossing a page boundary.

### Function extent and exact-entry dispatch

`IrFnEntry {addr, end}` lists only **callable entry points**. Extents may
overlap because the discovery assigns shared cold blocks to several functions;
`n3ds_ir_end()` is an **exact-entry** lookup, so a call is never routed to a
containing function by accident. The entry's own `end` is the frame limit.

`n3ds_rt_dispatch()` resolves:

1. AOT `FnEntry` table (including `0xff000000+` Win32 thunks);
2. exact IR entry;
3. otherwise `unknown function/import` (direct) or `unknown indirect function`
   (indirect).

`rt_call_ic` caches only AOT entries. An interpreter target is executed
immediately and the per-site cache is left `NULL`; generated code re-enters
`rt_call_ic`. No generated code was changed.

The hybrid build links a weak table generated by
`tools/ir_select.py --emit-ir-table`:

```c
const IrFnEntry g_ir_table[] = { ... };
const unsigned g_ir_count = ...;
```

Emit it as `<TESV_GEN>/tesv_ir_table.c` so the existing `tesv_*.c` wildcard
compiles it; `n3ds_lift_bind_modules()` binds it automatically on ELF/3DS. A
pure-AOT build leaves both symbols undefined.

## 5. Opcode coverage and the fallback

v2 implements the integer subset **plus x87 and the high-impact SSE/SSE2
subset**. The real unsupported-opcode histogram drove the order:

| Class | Before v2 (4,555 failed functions) | After v2 (10 failed functions) |
|---|---:|---:|
| x87 (`d8–df`) | 4,341 functions / 150,638 insns | implemented |
| SSE/SSE2 | 714 functions / 61,887 insns | implemented subset |
| MMX | 0 | still rejected |
| "block has no terminator" | 0 | 10 functions (shared cold-block edges) |

The v1 histogram's top x87 forms were `d9/0 fld m32` (44,342), `d9/3 fstp m32`
(30,543), `dd/3 fstp m64`, `df/4 fnstsw ax`, `d8/1 fmul`, `d8/0 fadd`,
`d9/5 fldcw`, `de/0 fiadd`, `db/0 fild`, `d9/1 fxch`, `d9/2 fst`, and
`d9/f*` transcendental/rounding forms. The SSE top was `movaps` (0x28/0x29),
`shufps` (0xc6), `mulps/addps/subps` (0x59/0x58/0x5c), `movups` (0x10/0x11),
`xorps/andps/andnps/orps` (0x57/0x54/0x55/0x56), `unpcklps/hps` (0x14/0x15),
`cmpps` (0xc2), `movlpd` (0x12), `minps` (0x5d), `rcpps/rsqrtps` (0x53/0x52),
`maxps` (0x5f), `movmskps` (0x50), plus `movd/movq` (0x6e/0x7e/0xd6),
`psrlq/psllq` (0x71–0x73 and variable shifts), `pshufd` (0x70), `pcmpeq*`,
`pcmpgt*`, `paddq/psub*`, `pmaxsw/pminsw`, `pmullw/pmulhw`, `pavgb`,
`pmaddwd`, `pmovmskb`, `pextrw/pinsrw`, `cvtdq2pd`, `cvttsd2si/cvtsd2si`,
`cvtsi2sd`, `ucomiss/comiss`, `ldmxcsr/stmxcsr` and fences.

Everything else still traps explicitly, never silently:

```
Guest fault at 00501637: IR: unsupported opcode push es at 00501637
```

Not implemented: MMX (`mm0..mm7`), SSE3/SSSE3/SSE4 beyond the listed ops,
AVX, `in/out`, `hlt`, `cli/sti`, far calls/jumps, `iretd`, `enter`, `bound`,
`arpl`, `daa/das/aaa/aas/aam/aad`, 16-bit addressing, `gs`, `mov to sreg`.

**Fallback:** the selection workflow asks the real decoder which profile
functions v2 cannot decode, then forces exactly those AOT:

```
python tools/ir_measure.py --profile profile.csv --functions slice.json \
    --fail-list v1_fail.txt
python tools/ir_select.py profile.csv slice.json --out sel \
    --limit 32 --force-aot v1_fail.txt --emit-ir-table sel/ir_table.c --validate
```

The real 10 forced functions cost 34,514 x86 bytes / 10,078 instructions /
0.24 MiB AOT. They fail on a shared-cold-block terminator, not on an opcode.

### `cmc` differential regression — fixed

An earlier run found a real AOT miscompile: `FLAG_EFFECT["cmc"]` was
`(0, F_CF)`, so the liveness pass dropped `stc` in `clc; stc; cmc` and AOT
returned CF=1 where x86 (and the IR) returns CF=0. The codegen owner fixed it
to `(F_CF, F_CF)`. The sequence is now a **permanent regression**
(`tests/ir_equiv.c:test_cmc`); it reports:

```
ok cmc: clc;stc;cmc agrees (lift.py FLAG_EFFECT["cmc"] fixed)
```

If the bug returns, the test prints `KNOWN AOT BUG cmc ...` and fails the
normal pass path (the previous "known expected failure" branch remains only as
a diagnostic for that regression).

## 6. Selection tool

```
python tools/ir_select.py PROFILE.csv FUNCTIONS.json --out DIR [options]
  --rank-by time|calls|weighted_insns
                         default: time with complete time data, else calls
  --limit N             maximum AOT functions
  --max-x86-bytes B     AOT budget in x86 bytes
  --aot-bytes B         AOT budget in projected ARM bytes
  --aot-bpi 25.35       projected AOT density (B per x86 instruction)
  --ir-bpi 8.26         projected IR density (B per x86 instruction)
  --insn-per-byte .28   x86 instructions per byte (growth12 unique pass)
  --exe TESV.exe        count profile/AOT instructions with capstone
  --force-aot FILE      addresses that must stay AOT (e.g. unsupported list)
  --emit-ir-table FILE  C table of exact IR entry points
  --validate            run tools/ir_validate.py on the emitted files
```

The profile remains backward compatible with `addr,hits`. It also accepts a
third measured-time column, either `addr,hits,time_ns` or `addr,hits,ticks`:

- complete time data makes `time` the default ranking mode;
- `--rank-by time` **fails explicitly** if the column is absent or partial;
- `--rank-by calls` keeps the old `(-hits, address)` order;
- `--rank-by weighted_insns` ranks by `hits × instruction_count` and is
  explicitly labelled a **proxy**, not measured time;
- ties are always broken by address; `aot.txt` stays address-sorted for a
  stable build input.

`manifest.json` records `ranking_mode`, `ranking_basis`,
`weighted_insns_is_proxy`, `hardware_time_claim` (always false here),
`profile_time_unit/source/data`, per-function `measured_time`/`time_ns`/`ticks`
or an explicit null, `covered_time_share`, and `aot_ir_time_split`. When time
is missing, the time split is `{"basis":"unavailable",...}` rather than a
fabricated estimate. A separate `aot_ir_weighted_insns_split` records the
proxy work split. The time split covers recorded profile functions only;
forced functions absent from the profile have unknown time, not zero.

Real post-VA profile (`profile_post_va_163.csv`, 163 functions / 936 calls;
`growth12/iter01`) has **no time column**, so it remains explicitly
call-ranked:

```
ranking: calls
measured time: unavailable (missing); covered time share is null, not inferred
AOT: 32 functions, 673 calls (71.90%), 36,653 x86 bytes, 10,871 instructions
projected AOT: 0.26 MiB
forced AOT: 10 functions, 34,514 x86 bytes, 10,078 instructions, 0.24 MiB
IR: 25,328 functions, 2,873,609 union x86 bytes, ~792,146 instructions,
    6.24 MiB at 8.26 B/insn
slice projected total: 6.50 MiB
closure projection: 100% IR 22.8 MiB / 20% AOT 32.3 MiB / 50% AOT 46.5 MiB
partition: OK
```

Two profile addresses are not present in `functions.json` (reported, not
silently dropped). The 10 forced functions are large CRT/math functions that
the profile also calls; keeping them AOT is the honest 0.24 MiB cost.

`tests/ir_select_rank_test.py` proves on a synthetic profile that time ranking
selects a different set from call ranking, that `ticks` and headerless
three-column input work, that `weighted_insns` is labelled a proxy, and that
missing/partial time fails explicitly for `--rank-by time`.

**No hardware timing is claimed.** The 5.24× IR/AOT ratio in §9 is Azahar
emulator evidence only. A future ARM11 profile can supply `time_ns` or `ticks`
and switch the selection to measured-time ranking without changing the
partition validator.

The earlier "111.11 MiB" projection came from multiplying x86 *bytes* by a
bytes-per-*instruction* density. The corrected tool measures instructions
(capstone when the PE is available, otherwise the measured 0.2757 insn/byte
proxy) and reports 6.50 MiB for the slice / 32.3 MiB for the 20 %-AOT closure
partition.

## 7. Partition validation

`tools/ir_validate.py` checks an emitted `aot.txt` / `ir_table.c` /
`manifest.json` triple:

- AOT sorted and unique; IR exact entries sorted and unique;
- no address in both tiers;
- every entry exists in `functions.json`; every IR `end > addr`;
- manifest counts agree with the files;
- the union of IR ranges equals the manifest's `ir_x86_bytes` (shared basic
  blocks counted once).

`tests/ir_partition_test.py` runs the synthetic valid + five broken cases
(duplicate AOT, AOT/IR overlap, duplicate IR entry, bad end, unknown
function), then validates the real `hybrid_select_v2` selection. It is part of
`run_host_tests.ps1`.

## 8. Equivalence tests

`tests/ir_equiv.c` + `tools/ir_make_fixture.py`:

- a self-authored i386 corpus is assembled at `0x501000`; the AOT tier is
  generated from those exact bytes by the real `staged/lift.py`;
- each function runs from identical CPU/memory state through AOT and IR; the
  test compares the complete `CPU` struct (GPRs, `df`, `eflags_hi`, the x87
  stack/top/condition codes, `mxcsr`, XMM/MMX registers), the result and all
  guest writes;
- the existing generated `D3DXMatrixMultiply` pilot fixture is called from an
  IR function and compared with AOT;
- an AOT caller calls an IR-only helper which calls an AOT callee
  (AOT→IR→AOT), compared with the pure-AOT path;
- an unmapped load is compared for the exact fault text;
- an AOT-supported `push es` must raise the exact
  `IR: unsupported opcode push es at …` fault in IR;
- long integer, x87 and SSE loops (`bench`, `bench87`, `bench128`) exercise
  the tiers over thousands of instructions.

Output (current fixture):

```
equiv: 22 AOT functions, IR helper 00501850..00501860
  ok arith / logic / shifts / muldiv / byteops / memops / strings / flags
  ok loopback / direct / indirect / pilotcall / x87ops / sseops
  ok bench / bench87 / bench128          4 inputs each
  ok cmc: clc;stc;cmc agrees (lift.py FLAG_EFFECT["cmc"] fixed)
  ok trap: both tiers: Guest fault at deadbe00: unmapped guest memory
  ok unsupported: Guest fault at 00501637: IR: unsupported opcode push es at 00501637
  ok aot->ir->aot: result 00000086, IR blocks 39 -> 41
IR equivalence PASS
```

## 9. ARM11/Azahar benchmark

`source/ir_bench_main.c` is built with:

```
make TARGET=Skyrim_N3DS_irbench BUILD=build_irbench IRBENCH=1
tools/run_azahar.ps1 -ThreeDsx Skyrim_N3DS_irbench.3dsx -Name irbench
```

(The Isolated build uses `BUILD=build_irbench`; no shared Skyrim_N3DS_tesv
artifact is touched.) It verifies every workload AOT-vs-IR first, then times
17 functions with long integer, x87 and SSE loops, 2,000 repetitions, and 200
cold-IR repetitions. It writes `sdmc:/ir_bench.txt` and the console, and never
reports a timing if verification failed.

Measured on Azahar 2126.2 (host Ryzen 7 7700, OpenGL, ARM11 emulation; *not*
New 3DS hardware):

```
IRBENCH functions=17 reps=2000 cold_reps=200 verify=ok
AOT       total_ticks=360380896 us_per_pass=672.1 us_per_fn=39.53
IR warm   total_ticks=1887886528 us_per_pass=3520.7 us_per_fn=207.10
IR cold   total_ticks=210214013  us_per_pass=3920.3 us_per_fn=230.60
decode    us_per_pass=399.6 (cold - warm)
ratio     IR warm / AOT = 5.24x
```

The IR is **5.24× slower** than AOT on this workload, with decode costing
~400 µs per pass (17 functions, 1 MiB cache). This is emulator evidence, not
a hardware claim; the design estimate of 25–45 ARM instructions per guest
instruction is consistent with the measured ratio but must be confirmed on a
New 3DS.

The ARM interpreter `.text` (not object file size) measured with
`arm-none-eabi-size` is **55,338 B (0.05 MiB)** at `-O2`:
`ir_core.c` 312 B, `ir_decode.c` 15,460 B, `ir_exec.c` 39,566 B. The full
`irbench` ELF `.text` is 220,540 B (0.21 MiB) including libctru/console.

## 10. Integration checklist

1. `ir_core.c`, `ir_decode.c`, `ir_exec.c` are in the ARM object lists (done
   for lift and TESV-slice modes).
2. Emit the IR table for the non-AOT functions:
   `tools/ir_select.py … --emit-ir-table <TESV_GEN>/tesv_ir_table.c --validate`.
3. Optionally size the cache in `main`: `n3ds_ir_cache_init(8u << 20);`
   (default 1 MiB, allocated lazily).
4. Keep the 10 terminator/shared-block functions AOT via `--force-aot`
   from `ir_measure.py --fail-list`.
5. The weak `g_ir_table` binding is automatic on ELF/3DS; a pure-AOT build
   behaves exactly as before.

## 11. Full hybrid ARM link (`irfull`)

Stable inputs:

```
slice:    E:\Skyrim_N3DS_build\analysis\growth12\iter01\slice_functions.json
profile:  E:\Skyrim_N3DS_build\analysis\profile_post_va_163.csv
forced:   E:\Skyrim_N3DS_build\analysis\ir_v7_fail.txt
select:   E:\Skyrim_N3DS_build\irfull\select
gen:      E:\Skyrim_N3DS_build\irfull\gen
```

Reproducible commands:

```
python tools/ir_select.py E:\Skyrim_N3DS_build\analysis\profile_post_va_163.csv ^
  E:\Skyrim_N3DS_build\analysis\growth12\iter01\slice_functions.json ^
  --out E:\Skyrim_N3DS_build\irfull\select --limit 32 ^
  --force-aot E:\Skyrim_N3DS_build\analysis\ir_v7_fail.txt ^
  --emit-ir-table E:\Skyrim_N3DS_build\irfull\select\ir_table.c --validate

python tools/ir_make_hybrid.py ^
  --functions E:\Skyrim_N3DS_build\analysis\growth12\iter01\slice_functions.json ^
  --selection E:\Skyrim_N3DS_build\irfull\select ^
  --exe E:\skyr\TESV.exe --out E:\Skyrim_N3DS_build\irfull\gen

$env:PORTLIBS='/opt/devkitpro/portlibs/3ds'
powershell -File tools/build.ps1 -Mode tesv ^
  -TesvGen E:\Skyrim_N3DS_build\irfull\gen -Isolated irfull

powershell -File tools/run_azahar.ps1 ^
  -ThreeDsx Skyrim_N3DS_tesv_irfull.3dsx -Name irfull ^
  -TimeoutSec 45 -AllowFaultReport
```

Linked evidence (`arm-none-eabi-nm -S`):

| Symbol | Size | Meaning |
|---|---:|---|
| `g_fn_table_tesv` | 256 B | 32 AOT entries |
| `g_ir_count` | 4 B | value `0x000062f0` = 25,328 |
| `g_ir_table` | 202,624 B | 25,328 × 8 B IR entries |

The PE entry `0x00f568a1` is **not** in the 32-entry AOT table and **is** in
the IR table. `n3ds_boot_prepare()` accepts the session only if
`n3ds_lift_has_function(entry)` succeeds; therefore the successful run proves
the runtime did not fall back to pure AOT.

Artifacts:

| Artifact | Bytes |
|---|---:|
| ELF `.text` | **1,438,312 (1.37 MiB)** |
| ELF `.data` | 7,328 |
| ELF `.bss` | 941,844 |
| ELF file (debug included) | 6,244,244 |
| `.3dsx` | **1,454,000 (1.39 MiB)** |
| `dist/Skyrim_N3DS_tesv_irfull/build.log` | 5,103 |

The 30–35 MiB projection is a *runtime representation* projection for the
2.9 M-instruction closure. The hybrid link does not carry that IR stream in
the executable: it carries 202,624 B of exact-entry metadata plus the 55 KiB
interpreter, and decoded blocks live in the bounded RAM arena (1 MiB default).
Thus the linked ARM text is 1.37 MiB, far below the target, while the full
closure remains available through the guest image + IR table. The 32 selected
AOT functions project to 0.26 MiB and the 10 forced functions to 0.24 MiB.

Azahar result (`build_azahar/20261124-164626-irfull/`):

```
run rc=0
guest_used=32120832 steps=200000
Guest ExitProcess with status 0
exit code=0 from=00f5894a
executed functions: 19, calls: 493
```

There was **no guest fault and no IR-specific trap**. `run_azahar.ps1`
nevertheless returned 1 because its pass/fail protocol requires either a
`PASS` marker or a `Guest fault at ...` line; this run was a clean
`ExitProcess(0)`. The SD log and emulator log were kept bounded (6.7 KiB and
10 KiB). No `hwtest` artifact was used or modified.

A host run of the same `irfull/gen` with `TESV_MEMPROBE=1` passes the memory gate
and reaches `Guest fault at 00b2a970: unknown indirect function`, with 97,536 KiB
commit promise and 68 KiB physically backed. This proves deep AOT↔IR execution
on the host.

The isolated `irprobe` ARM build (`tools/build.ps1 -Mode tesv -TesvGen
E:\Skyrim_N3DS_build\irfull\gen -Isolated irprobe -ForceMemProbe`) reaches the
same `0x00b2a970` fault in Azahar and reports:

```
IR cache: blocks=859 bytes=94584 hits=18468 misses=859 evicted=0
guest_used=35583424 steps=200000
```

Evidence: `build_azahar/20261124-171506-irprobe/`. The harness printed an overall
FAIL only because a concurrent armtime benchmark wrote `Skyrim_N3DS.log`; the
TESV log itself contains the expected diagnostic fault and nonzero IR-cache
counters.

## 12. Limits and honest caveats

- **Coverage is 25,350/25,360 functions and 99.63 % of the union x86 bytes.**
  The 10 remaining functions fail on a shared-cold-block terminator, not on an
  unimplemented instruction; they are forced AOT at 0.24 MiB.
- MMX and SSE3/SSSE3/SSE4 operations beyond the listed set still trap
  explicitly. The histogram showed no MMX use in this slice, but other game
  code may use it.
- Instruction-level throughput is measured on **Azahar**, not hardware. The
  5.24× warm ratio is the useful number until someone runs `ir_bench` on a New
  3DS.
- The decode cache is single-threaded and has no self-modifying-code
  invalidation, matching the AOT tier.
- The `cmc` AOT bug is fixed and guarded by a permanent differential
  regression.
