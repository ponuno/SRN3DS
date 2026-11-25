# Hybrid execution — AOT ARM + IR interpreter

Status: **proposal with measured inputs**, not implemented. This document records
the measured size problem, the corrected numbers after a real 9,000-function
build, and the plan for code that is not statically reachable. Numbers marked
*measured* come from this workspace; everything else is an estimate.

**Correction (2026-11-24).** An earlier revision used 56.9 B/insn, calibrated on
one float-heavy D3DX function, and concluded that even the startup closure could
not fit. A real 9,000-function build measures **28.2 B/insn (8.77× expansion)**,
so the startup closure fits. The hybrid plan is still the answer for code beyond
the static closure, but it is no longer a prerequisite for startup.

## 1. The measured problem

| Quantity | Value | Source |
|---|---:|---|
| Closed startup closure (direct calls, `call [slot]`, TLS, immediates, data slots) | 24,550 functions | `tools/slice_pe.py` |
| x86 instructions in that closure | 2,900,299 | same |
| **Measured ARM `.text` for a 9,000-function slice** | **26,111,200 B (24.9 MiB)** | `dist/Skyrim_N3DS_tesv` (`-O2`, devkitARM 15.2.0) |
| Measured density | **28.2 B per x86 instruction**, 8.77× code expansion | 26,111,200 / 924,440 |
| Earlier calibration (one float-heavy D3DX math function) | 56.9 B/insn, 21.7× | `docs/MEMORY_BUDGET.md` §2 |
| TESV guest image | ~25.5 MiB | `tools/audit_game.py` |
| Guest heap/stack | 30 MiB budget (measured need 28.1 MiB total) | `tesv_slice_main.c` |
| libctru heaps for a diagnostic build | 34 MiB + 8 MiB linear | `tesv_slice_main.c` |

The single-function calibration was **2× pessimistic** for real startup code.
Recomputing the closed closure with the measured density:

| Item | Estimate |
|---|---:|
| Closed-closure ARM `.text` (2,900,299 × 28.2 B) | **~78 MiB** |
| rodata/data/bss + tables | ~1.5 MiB |
| Guest image + heap/stack | 30 MiB |
| linear heap (GPU) | 8 MiB |
| **Total** | **~117.5 MiB of 178 MiB** |

So the *startup closure* does fit the 178 MiB CIA mode with ~60 MiB to spare —
the opposite of what the 56.9 B/insn estimate implied. Two caveats:

- 24.9 MiB is measured on 9,000 of the 24,550 functions; the remaining 15,550
  are mostly CRT/x87 and are assumed to have the same density (a fair
  assumption, since the 9,000 slice already contains the CRT core, but it is an
  assumption).
- The closed session does not include the game's own world/menu code, which is
  not statically reachable from the entry and is the real consumer of budget.

The hybrid architecture is therefore **not required to fit the startup path**.
It remains the way to reach code that is not part of any static closure, and the
way to keep the ARM image small once engine code starts being reached.

## 1b. Measured: the executed set is tiny, its closure is not

Two profiled runs (9 000- and 24 765-function slices) executed **49** and
**99** distinct functions respectively before the first genuine fault. The deep
run made 572 calls; the busiest functions are CRT helpers:

| Address | Calls | x86 bytes |
|---|---:|---:|
| `0xf5d647` | 45 | 55 |
| `0xf5d79f` | 45 | 127 |
| `0xf5d818` | 45 | 29 |
| `0xf523fc` | 43 | 146 |
| `0xf5d120` | 42 | 70 |
| `0xf5d165` | 40 | 21 |
| `0xf67aa5` | 38 | 92 |

The top 32 functions cover **87.1 % of calls** and only ~3 KB of x86 code. At
the same time the *closure* of those 99 roots is **24 628 functions / 2.91 M
instructions / ~78 MiB** — generic CRT helpers call almost everything reachable.

Consequence for the design: choosing AOT code by *closure* saves nothing. The
AOT set has to be the **hot set measured at run time** (tens to low hundreds of
functions for startup), with everything else interpreted on first use. That is
exactly the three-tier plan, and the measurement now says why.

## 2. Three execution tiers

| Tier | What runs there | Representation | Expected share |
|---|---|---|---|
| AOT ARM | hot startup/engine functions chosen from a profile | current C→ARM path | 10–30 % of calls |
| IR interpreter | everything else, including code first seen at run time | decoded x86 → compact IR blocks, LRU cache in RAM | 70–90 % of calls, most of the code |
| Native reimplementation | math, string, memory, Win32, D3D9 | hand-written C | all API surface |

Dispatch uses the existing function table (`g_fn_table_tesv`). Each entry gains
a kind bit: `AOT` or `INTERP`. `rt_call_ic` and direct calls already go through
that table, so the dispatcher is a table lookup, not a new calling convention.
An address that is neither AOT nor decodable still produces the current explicit
`unknown function 0x…` fault.

No code paging from the SD card. The SD is the slowest part of the system and a
page fault in the middle of a render frame would be visible. Only *decoded IR*
is cached, and it can be dropped and re-decoded at any time.

## 3. Corrected memory budget (178 MiB, provisional)

| Component | Target | Notes |
|---|---:|---|
| Constant AOT ARM code + tables | 20 MiB | ≈ 740k x86 instructions at the measured 28.2 B/insn |
| Decoded IR cache | 8 MiB | ≈ 700k instructions at ~12 B/insn; evictable |
| Guest images (TESV + 4 DLLs) | 32 MiB | measured 27.9 MiB for the current set |
| Game heap (allocators, ESM/BSA indexes) | 40 MiB | **estimate**; Skyrim's PC peak is hundreds of MiB |
| Textures | 28 MiB | **optimistic**; requires downscaling + streaming, unproven |
| Geometry | 16 MiB | estimate |
| Audio | 6 MiB | estimate |
| Runtime, stacks, libctru, linear heap | 8 MiB | linear heap pinned separately |
| Reserve | 20 MiB | fragmentation, driver, debug |
| **Total** | **178 MiB** | arithmetic only; nothing below the line is measured yet |

The heap/texture/geometry/audio rows are placeholders. The first three must be
measured from real TESV allocation traces before this table can be called a plan
rather than a sketch.

## 4. Making the AOT part smaller (independent of the interpreter)

Ordered by measured or expected effect per unit of risk:

1. **Guest memory fast path.** Today every guest access calls
   `n3ds_guest_ptr(addr,n)` with a region scan. Map the guest image + heap into
   one contiguous window and emit `ldr/str` from a base register, checking only
   when an access crosses a page. Expected 1.4–1.8× smaller text on
   memory-heavy code.
2. **Lazy flags.** `sub32/logic32/...` compute `zf/sf/of/cf` for every
   instruction even when nothing reads them. Emit flag materialisation only for
   the consumers (`jcc`, `setcc`, `adc/sbb`, `cmov`). Expected 1.2–1.4×.
3. **Register allocation per basic block.** Keep the eight x86 GPRs in ARM
   registers inside a block; spill only at calls and block boundaries. Today
   every call does a full spill/reload. Expected 1.2–1.5×.
4. **One C function per basic block** instead of per x86 function: removes
   prologue/epilogue per call and lets the compiler share tails. Expected
   1.05–1.15×.

A 2× total improvement (56.9 → ~28 B/insn) would put the *closed* startup
closure at ~79 MiB and make AOT-only feasible for the startup path; the
interpreter is what makes the rest of the game reachable at all.

## 5. IR and interpreter shape

- Decode on demand, one basic block at a time; a block is a fixed-size header
  plus an array of 8-byte instructions (opcode, dst, src, imm) and a terminator.
- Cache: direct-mapped 8 MiB arena with LRU eviction; a block key is its guest
  address. Decoding is deterministic, so eviction never changes behaviour.
- Interpreter: a computed-goto loop over opcodes. Estimated 25–45 ARM
  instructions per guest instruction including dispatch.
- Guest memory access goes through the same fast path as AOT code (one base +
  bounds check), so both tiers share the memory model.

## 6. Performance estimate (must be measured, not assumed)

ARM11 MPCore on New 3DS runs at 268/804 MHz. A realistic interpreter budget is
200–350 M ARM instructions/s, i.e. **5–12 M x86 instructions/s**. At that rate
the 2.9 M-instruction startup closure costs well under a second of pure
interpretation, and a 50 M-instruction dynamic startup path costs 5–10 s. World
streaming and rendering remain out of reach at these numbers; they are not part
of the near-term gate.

## 7. Order of work

1. **Profile the executed set and call frequency** (implemented: `RT_PROF`).
2. Differential test AOT vs interpreter on the D3DX pilot before the interpreter
   is allowed near TESV.
3. Guest-memory fast path, then lazy flags, then register allocation.
4. IR + interpreter for the cold tier.
5. AOT-set selection from a recorded profile; the selection is a build input, so
   builds stay reproducible.
6. Re-measure: executed set, ARM text, interpreter throughput, fault chain.

## 8. Risks

- **Two semantics.** AOT and interpreter must agree bit-for-bit; the pilot
  differential test is the guard. Any divergence is a bug, not a tuning knob.
- **Profile overfitting.** A startup profile says nothing about a dungeon. That
  is acceptable because unknown functions are interpreted, never skipped; it only
  costs speed.
- **Interpreter speed.** If the ARM11 lands near 5 M x86 insn/s, startup is still
  diagnosable but a playable build is not. This must be measured on hardware
  before more of the game is attempted.
- **Renderer gate.** None of this addresses D3D9 SM3; that blocker is orthogonal
  and comes first for any visible output.
