# Legacy x86 ops — TESV `junk aas` / `junk daa`

Status: **AAS/DAA analysis is read-only; no AAS/DAA implementation**. The
2026-11-27 MMX addendum is in §11. Written 2026-11-24 while the fault-driven
TESV slice growth was still running.

## 1. Conclusion

The two rejections in the active Growth17 slice are **not executed AAS/DAA
instructions**. They are bogus function entries created when string data was
mistaken for a function-pointer table:

| Rejected entry | Numerical data value | Actual bytes at that VA |
|---|---|---|
| `0x00636573` | `"sec\0"` | middle of a `mov` immediate; decodes as AAS at `0x00636579` |
| `0x00676f46` | `"Fog\0"` | middle of a `mov` immediate; decodes as DAA at `0x00676f46` |

The coherent owning functions, `0x006364a0` and `0x00676f30`, are present in the
generated function table and lift successfully. Neither bogus entry has a direct
caller. Growth17's first observed fault is elsewhere (`0x0042ceb0`, unknown
indirect function), not at either legacy opcode.

**Recommendation:** fix data-root/function-start validation and remove the bogus
roots. Do not add AAS/DAA merely to make this rejection report green. Implement
their real semantics later as a small, independently tested completeness item;
that implementation is specified in §5–§7.

A concurrent newer snapshot (`g24`) contains two additional bogus entries of the
same kind. They do not change the conclusion; see §3.3.

## 2. Evidence snapshot

Inputs:

- Executable: `E:\skyr\TESV.exe`
  - SHA256 `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`
- Discovery: `E:\Skyrim_N3DS_build\analysis\tesv\functions.json`
  - SHA256 `1fc28ce90d668d635ace8fd641d95720782e24c0f64eaf08cbaf8bf170d15b6d`
- Active Growth17 rejection report:
  `E:\Skyrim_N3DS_build\analysis\growth17\gen01\tesv_rejected.json`
  - SHA256 `80dcb9e294f69c7d4406b1965e998f451a7de846490b28766a3d39519d49c153`
- Newest rejection file seen during analysis:
  `E:\Skyrim_N3DS_build\analysis\g24_gen\tesv_rejected.json` (17:34 local)
  - SHA256 `f5d8a301d03ff61d01d0e302552aab7dc3175c5a9b473cc46bb2eaf905f0d17c`

Growth17 iteration 1 selected 27,598 functions, lifted 27,596, and rejected only:

```json
{
  "0x636573": "junk aas",
  "0x676f46": "junk daa"
}
```

The files can advance while growth runs; the addresses and byte evidence above
are tied to the hashes in this section.

## 3. Exact sites and disassembly

### 3.1 Growth17 AAS: rejected entry `0x00636573`

Discovery/slice entry: `0x00636573`

- Listed blocks: 15; union `[0x00636573, 0x006366ba)`.
- First block: `[0x00636573, 0x00636595)`.
- Offending instruction: **AAS at `0x00636579`**, not at the function entry.

Decode from the rejected entry:

```asm
00636573: 86 ba 01 a4 70 7d       xchg byte ptr [edx + 0x7d70a401], bh
00636579: 3f                      aas
0063657a: c7 05 04 86 ba 01 ...   mov dword ptr [0x01ba8604], 0x3c23d70a
00636584: e8 47 d0 ff ff          call 0x006335d0
00636589: 89 44 24 14             mov dword ptr [esp + 0x14], eax
0063658d: 85 c0                   test eax, eax
0063658f: 0f 84 cd 00 00 00       je 0x00636662
```

The coherent owner is `0x006364a0`, union `[0x006364a0, 0x00636718)`. Its block
starting at `0x0063654d` decodes correctly:

```asm
00636566: c7 05 fc 85 ba 01 9a 99 d9 3f
                               mov dword ptr [0x01ba85fc], 0x3fd9999a
00636570: c7 05 00 86 ba 01 a4 70 7d 3f
                               mov dword ptr [0x01ba8600], 0x3f7d70a4
0063657a: c7 05 04 86 ba 01 0a d7 23 3c
                               mov dword ptr [0x01ba8604], 0x3c23d70a
```

Thus byte `0x3f` is the high byte of the IEEE-754 immediate `0x3f7d70a4`, not an
opcode on the real instruction stream.

How it entered the slice:

```asm
00fc3d30: 68 24 74 07 01    push 0x01077424
```

`0x01077424` contains the bytes `73 65 63 00` (`"sec\0"`). Interpreted as a
little-endian dword this is `0x00636573`, which is also a discovery “function”.
`tools/slice_pe.py` admitted it as a data-table root.

### 3.2 Growth17 DAA: rejected entry `0x00676f46`

Discovery/slice entry and only block: `[0x00676f46, 0x00676f54)`.

```asm
00676f46: 27                      daa
00676f47: 01 89 46 1c e8 80       add dword ptr [ecx - 0x7f17e3ba], ecx
00676f4d: 3f                      aas
00676f4e: 3d 00 8b c6 5e          cmp eax, 0x5ec68b00
00676f53: c3                      ret
```

The first rejection is DAA; the same bogus decode also contains an AAS at
`0x00676f4d`.

The coherent owner is `0x00676f30`, `[0x00676f30, 0x00676f54)`:

```asm
00676f30: 33 c0                   xor eax, eax
00676f32: 56                      push esi
00676f33: 8b f1                   mov esi, ecx
00676f35: 8d 4e 20                lea ecx, [esi + 0x20]
00676f38: 89 46 08                mov dword ptr [esi + 8], eax
00676f3b: 89 46 0c                mov dword ptr [esi + 0xc], eax
00676f3e: 89 46 10                mov dword ptr [esi + 0x10], eax
00676f41: c7 46 14 fc 22 27 01    mov dword ptr [esi + 0x14], 0x012722fc
00676f48: 89 46 1c                mov dword ptr [esi + 0x1c], eax
00676f4b: e8 80 3f 3d 00          call 0x00a4aed0
00676f50: 8b c6                   mov eax, esi
00676f52: 5e                      pop esi
00676f53: c3                      ret
```

The false DAA byte `0x27` is inside the immediate `0x012722fc`; the false AAS
byte `0x3f` is inside the call displacement.

How it entered the slice:

```asm
00fc4410: 68 28 86 07 01    push 0x01078628
00fc4415: 68 10 86 07 01    push 0x01078610
```

`0x01078628` contains `46 6f 67 00` (`"Fog\0"`), i.e. the false pointer
`0x00676f46`. A neighboring initializer-like wrapper also references
`0x01078608`, whose run eventually reaches the same `"Fog\0"` dword.

### 3.3 Newer `g24` snapshot

The newest rejection report contains four AAS/DAA-related bogus functions:

| Rejected entry | Offending site | Listed union | Coherent owner | False-byte source |
|---|---|---|---|---|
| `0x0042424f` | DAA at entry | `[0x0042424f, 0x004242d8)` | `0x00424200` | byte inside `call 0x006a2f10` at `0x0042424c` |
| `0x00636573` | AAS at `0x00636579` | `[0x00636573, 0x006366ba)` | `0x006364a0` | float immediate at `0x00636570` |
| `0x00676f46` | DAA at entry | `[0x00676f46, 0x00676f54)` | `0x00676f30` | pointer immediate at `0x00676f41` |
| `0x00676f6c` | AAS at entry | `[0x00676f6c, 0x00741ea3)` | `0x00676f60` | byte inside `call 0x00a4af50` at `0x00676f6a` |

`g24_roots.txt` explicitly contains `0x0042424f`, `0x00636573`, and `0x00676f6c`
(lines 2228, 9591, and 9729 in that snapshot). `0x00676f46` is admitted as a data
root. Therefore fixing the slicer alone will not clean an already-expanded root
file; the bogus explicit roots must also be removed after growth stops.

## 4. Reachability: dead decode artifacts, not a current blocker

### Static evidence

A direct-call/jump closure from the real entry `0x00f568a1` contains 13,401 of
the 90,900 discovered functions. The coherent owners are in that closure:

```text
0x00f568a1 -> 0x0069d1d0 -> 0x0069cbd0 -> 0x0069c930 -> 0x0069bdf0
           -> 0x0069b4c0 -> 0x00694ef0 -> 0x00637fa0 -> 0x006364a0

0x00f568a1 -> 0x0069d1d0 -> 0x00699530 -> 0x0067b6d0 -> 0x0067b230
           -> 0x00676f30
```

`0x00676f60` is reachable by the second path as well. These paths prove static
reachability of the real functions, not execution of the false entries.

The false entries `0x00636573`, `0x00676f46`, `0x00676f6c`, and `0x0042424f`
have **zero direct callers** in the discovered image and are not in the entry's
direct closure. The initializer-like wrappers that supplied the string pointers
(`0x00fc3d30`, `0x00fc43f0`, `0x00fc4410`) are also not in that direct closure;
their addresses occur in pointer tables at `0x0106b48c`, `0x0106b560`, and
`0x0106b564`. They were admitted as static roots, not observed executing the
strings as code.

### Dynamic evidence

- Growth17 iteration 1 first fault: `0x0042ceb0: unknown indirect function`.
- No observed guest fault names either legacy site.
- The run's `tesv_profile.csv` contains only its header, so it cannot prove that
  the real owners or initializer wrappers executed.
- Later `g23`/`g24` diagnostic logs stop at the pilot basic-block budget, also
  without a legacy-opcode fault.

Assessment: **dead/misaligned legacy decodes with high confidence**. They are not
the current startup blocker. A real AAS/DAA on a valid instruction boundary
would be a future correctness issue, but none is confirmed by this analysis.

## 5. Correct x86-32 semantics

Both instructions are one-byte, operand-less instructions valid in legacy and
compatibility mode and invalid in 64-bit mode. `LOCK` causes `#UD`. This guest is
x86-32, so no mode check is needed in the lifter, but a locked form must remain an
explicit error.

### DAA (`0x27`) — Decimal Adjust AL After Addition

Intel pseudocode:

```text
old_AL  = AL
old_CF  = CF
CF = 0
if ((AL & 0x0f) > 9) or AF:
    AL = AL + 6
    CF = old_CF or carry_from(AL + 6)
    AF = 1
else:
    AF = 0
if old_AL > 0x99 or old_CF:
    AL = AL + 0x60
    CF = 1
else:
    CF = 0
```

- **AL:** final adjusted packed-BCD byte.
- **AH:** unchanged.
- **CF:** decimal carry out of either digit; always assigned as above.
- **AF:** 1 iff the low-nibble `+6` adjustment was taken.
- **ZF/SF/PF:** set from the final AL (`PF` is even parity of the low byte).
- **OF:** undefined.

### AAS (`0x3f`) — ASCII Adjust AL After Subtraction

```text
if ((AL & 0x0f) > 9) or AF:
    AX = AX - 6
    AH = AH - 1
    AF = 1
    CF = 1
    AL = AL & 0x0f
else:
    CF = 0
    AF = 0
    AL = AL & 0x0f
```

- **AL:** low nibble only; top four bits cleared in both paths.
- **AH:** decremented modulo 256 only on the borrow path; otherwise unchanged.
- **AX:** the combined result of the AL/AH updates.
- **CF/AF:** 1 on the decimal-borrow path, otherwise 0.
- **OF/SF/ZF/PF:** undefined.

### Undefined-flag policy for this port

Choose a deterministic policy: **clear undefined output flags**. DAA therefore
clears OF; AAS clears OF, SF, ZF, and PF. This is architecturally permitted.
Differential tests must mask architecturally undefined bits rather than demand
one hardware-specific value.

## 6. Existing CPU/lifter gaps and implementation shape

Current relevant code:

- `staged/lift.py:35-40` puts `daa` and `aas` in `JUNK`;
  `staged/lift.py:828-829` turns that into `Unsupported("junk ...")`.
- `staged/x86.h:63` defines `Flags { cf, zf, sf, of, pf }` — **there is no AF**.
- Arithmetic helpers at `staged/x86.h:67-95` do not compute AF.
- `sahf`, `lahf`, `pushfd`, and `popfd` in `staged/lift.py:1143-1160` omit EFLAGS
  bit 4 (AF).
- The IR decoder explicitly rejects `0x27`/`0x3f`
  (`source/ir_decode.c:94-97`); `source/ir_core.h` has no DAA/AAS opcodes.

A correct implementation consequently needs more than two lifter branches:

1. **Add `uint32_t af` to `Flags`.** Initialize it with the existing `{0}` local.
2. **Compute AF where defined** in the shared arithmetic helpers:
   - add/adc: `(a ^ b ^ result) & 0x10`
   - sub/sbb: `(a ^ b ^ result) & 0x10`
   - inc/dec: `(a ^ result) & 0x10`
   - neg: `(a & 0x0f) != 0`
   - logic operations: clear AF under the deterministic undefined-bit policy.
3. **Wire AF through flag visibility:** `sahf` loads bit 4, `lahf` returns bit 4,
   and `pushfd`/`popfd` preserve/populate bit 4. Audit other partial flag writers
   so they preserve AF unless they intentionally assign/clear it.
4. **Add shared helpers** (suggested `daa8(uint8_t al, Flags *f)` returning AL and
   `aas16(uint16_t ax, Flags *f)` returning AX) beside the other helpers in
   `staged/x86.h`. Keep the old AL/CF values before modifying flags.
5. **AOT lowering:** remove `daa`/`aas` from `JUNK`; emit an AL write for DAA and
   an AX write for AAS using the shared helpers. Keep `lock daa`/`lock aas` as
   explicit unsupported/#UD cases. Add implicit EAX read/write to `insn_regs()`;
   Capstone 5 reports no implicit registers for these two opcodes.
6. **Lazy flags:** model DAA as reading CF/AF and writing all modeled flags; AAS
   reads AF and writes all modeled flags under the clear-undefined policy. This
   forces the preceding arithmetic writer to materialize AF.
7. **IR parity:** add `IR_OP_DAA`/`IR_OP_AAS`, decode `0x27`/`0x3f`, and execute
   them through the same `x86.h` helpers. Do not create a second semantic
   implementation in `source/ir_exec.c`.

## 7. Image-wide occurrence and impact estimate

Method: inspect the mapped `.text` range `[0x00401000, 0x0106a31a)` and replay all
discovery block ranges with Capstone.

| Measurement | Result |
|---|---:|
| Executable `.text` bytes | 13,013,786 |
| Raw byte `0x27` occurrences | 9,128 |
| Raw byte `0x3f` occurrences | 12,410 |
| Discovered functions | 90,900 |
| Distinct discovery block ranges replayed | 626,955 |
| Instructions decoded from those ranges | 4,097,998 |
| Distinct decodes labelled DAA | 25 |
| Distinct decodes labelled AAS | 31 |
| Confirmed valid-boundary AAS/DAA instructions | **0** |

Raw byte counts are not opcode counts: these bytes commonly occur in immediates,
displacements, pointer tables, strings, and data embedded in `.text`. Replaying
unvalidated discovery ranges produces 56 candidate labels, but inspection
explains the current-slice candidates and many others as bytes inside coherent
`mov`/`call`/`jmp` instructions. The remainder are in data-like or otherwise
unvalidated discovery regions. They are an **upper bound requiring better
function-boundary data**, not evidence of 56 real legacy instructions.

Practical impact:

- Growth17: 2 rejected entries out of 27,598 selected functions (~0.007%).
- `g24`: 4 AAS/DAA-related rejected entries out of 57,553 selected functions
  (~0.007%); 4 of 31 total rejection entries in that report.
- The real owners already lift, so dropping the bogus entries loses no coherent
  function.
- No code-volume or startup blocker is attributable to AAS/DAA. The material
  issue is false root admission, which can also admit other junk opcodes.

## 8. Minimal follow-up plan

### A. Unblock growth first (no legacy-opcode implementation required)

1. In `tools/slice_pe.py`, reject NUL-terminated printable string runs before
   treating a data address as a function-pointer table. The existing
   `printable_ascii_u32()` requires all four bytes to be printable, so it misses
   three-character strings such as `"sec\0"` and `"Fog\0"`.
2. Strengthen candidate validation: a data-root target must be a credible exact
   function start, not merely a discovery key. In particular, reject a candidate
   that lies inside an instruction decoded from a coherent containing function.
   The four sites above are all interior bytes of real `mov`/`call` streams.
3. Add synthetic slice tests for `"sec\0"`, `"Fog\0"`, and a legitimate adjacent
   function-pointer table. Assert that strings produce no roots while real
   tables still do.
4. After the running growth job reaches a safe boundary, remove the explicit
   false roots from expanded root files (`0x0042424f`, `0x00636573`, `0x00676f6c`)
   and regenerate. Do not edit those files while Growth17 is active.
5. Re-run the current slice and confirm that `0x006364a0`, `0x00676f30`,
   `0x00676f60`, and `0x00424200` remain lifted while the false entries disappear.

### B. Implement real legacy semantics independently

1. Add AF state/helper plumbing from §6 and focused helper tests.
2. Add AOT branches and lazy-flag metadata.
3. Add matching IR opcodes and execution.
4. Run the differential suites in both lazy/eager AOT modes and AOT-versus-IR.

## 9. Minimal test matrix

### Helper tests

- **DAA exhaustive:** all 256 AL values × old CF {0,1} × old AF {0,1} = 1,024
  cases. Check AL, AH preservation, CF, AF, ZF, SF, PF; ignore OF.
- **AAS exhaustive:** all 65,536 AX values × old AF {0,1} = 131,072 cases. Check
  AX, CF, AF; ignore OF, SF, ZF, PF.
- Include boundary vectors explicitly: AL `0x00`, `0x09`, `0x0a`, `0x99`,
  `0x9a`, `0xff`; AH `0x00`/`0xff`; old CF/AF combinations.
- Packed-BCD integration: add every pair `00..99`, then DAA; verify decimal
  result and carry.
- Unpacked-BCD integration: subtract every pair `0..9`, then AAS; verify decimal
  digit, borrow, and AH decrement.
- AF round-trip through `sahf`/`lahf` and `pushfd`/`popfd`.

### Lifter/IR tests

- Tiny valid functions containing DAA/AAS followed by `lahf`, `pushfd`, `jcc`,
  and `adc`/`sbb`, exercising lazy AF materialization.
- Compare eager, lazy, selective, and global-register AOT modes using the
  existing `tools/codegen_diff.py` harness.
- Compare AOT and IR using `tests/ir_equiv.c` / `tools/ir_make_fixture.py`.
- Assert `lock daa` and `lock aas` still fault explicitly.
- Assert the four false TESV entries are absent after the slicer fix, while the
  real owners remain present.

## 10. References

- Intel SDM, AAS operation and flags:
  https://github.com/intel/SDM/blob/main/data/sdm/AAS.docbook
- Intel SDM, DAA operation and flags:
  https://github.com/intel/SDM/blob/main/data/sdm/DAA.docbook
- Intel SDM, SAHF (AF is EFLAGS bit 4):
  https://github.com/intel/SDM/blob/main/data/sdm/SAHF.docbook
- Intel SDM index (the repository labels itself a preview; the linked Intel SDM
  book is the authoritative publication):
  https://intel.github.io/SDM/sdm.html
- Independent readable cross-check (unofficial mirror):
  https://www.felixcloutier.com/x86/aas and https://www.felixcloutier.com/x86/daa

## 11. MMX addendum (2026-11-27)

The legacy-opcode conclusions above are unchanged: no valid-boundary AAS/DAA
was found, and this MMX work does not implement them. The separate Havok AABB
blocker was MMX, not a legacy decimal instruction.

`tools/mmx_audit.py` replays `functions.json` block boundaries with Capstone.
The current full image has 82 distinct MMX sites (81 in
`0xd0c5e0..0xfc0c5f`, one outside). Havok's three clipping helpers
(`0xde8500`, `0xde8610`, `0xde86e0`) use `movq` load/store/register copies,
`pcmpgtw`, `paddw`, `psubw`, `pand`, and `emms`; `0xf7e184` adds `pavgb`, and
one shared CRT site uses `psrlq`. The exact count table and reproduction
command are in `docs/IR.md` §5.

The IR and staged AOT lifter now share `CPU.mm[]` plus `mmx_*` helpers for MMX
moves, logic, packed integer arithmetic/multiply/pack/compare and packed
shifts. `emms` uses `mmx_reset()` in both tiers; it clears modeled MMX state
and the tag word without resetting x87 TOP. `mmxops` in the self-authored
fixture differentially tests these forms over four random seeds and through
`tests/ir_havok_ext_smoke.c`. The rebuilt full container decodes `0xde8500`
(4 blocks / 101 instructions), all seven MMX-owning functions are `ok`, and the
invalid-MMX cross-check is empty; `havok_world` passes without the scalar AABB
override.

Remaining explicit MMX traps include `pshufw`, signed byte saturating
arithmetic, packed min/max and `maskmovq`. SSE3/SSSE3/SSE4/AVX and the legacy
AAS/DAA family also remain unsupported. No excluded renderer/BSA/DevCaps/F:
code was changed.
