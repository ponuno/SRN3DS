# `0xa43f80` / `0xa43fb5` — what the leaf actually does, and how to trace it

Measurement-only diagnostic. No guest semantics change without `TESV_A43F_TRACE`.

## 1. The real image, not `TESV.exe` on disk

The runtime executes the IR container input, not the file on disk. Disassembly is
taken from `E:\census_ir2\ir_container\_work\ir_container_input.bin`
(header `<IIII>` = magic `0x49524D31`, `lo=0x401000`, `size=0xc6931a`,
`entry=0x16314`; offset = `16 + VA - lo`). Reproduce with
`python tools/a43f_dis.py 0xa43f80 0xa43fe4`.

The function is a leaf: `ret 0x10` (4 stack arguments), no call, no loop, no
lock, no guest-memory *write* except the two caller out-slots.

```
00a43f80: 8b410c              mov  eax,[ecx+0xc]      ; A_hi
00a43f83: 8b5114              mov  edx,[ecx+0x14]     ; B_hi
00a43f86: 53 56 57            push ebx,esi,edi
00a43f88: 8b7108              mov  esi,[ecx+0x8]      ; A_lo
00a43f8c: 8b7910              mov  edi,[ecx+0x10]     ; B_lo
00a43f8f: 3bc2                cmp  eax,edx
00a43f91: 7740                ja   0xa43fd3           ; A > B  -> reject
00a43f93: 7204                jb   0xa43f99
00a43f95: 3bf7                cmp  esi,edi
00a43f97: 733a                jae  0xa43fd3           ; A == B (lo>=) -> reject
00a43f99: 8b5c241c            mov  ebx,[esp+0x1c]     ; arg3 = want_hi
00a43f9d: 3bc3                cmp  eax,ebx
00a43f9f: 7732                ja   0xa43fd3           ; A > want -> reject
00a43fa1: 8b442418            mov  eax,[esp+0x18]     ; arg2 = want_lo
00a43fa5: 7204                jb   0xa43fab
00a43fa7: 3bf0                cmp  esi,eax
00a43fa9: 7728                ja   0xa43fd3           ; A == want (lo>) -> reject
00a43fab: 3bda                cmp  ebx,edx
00a43fad: 7724                ja   0xa43fd3           ; want > B -> reject
00a43faf: 7204                jb   0xa43fb5
00a43fb1: 3bc7                cmp  eax,edi
00a43fb3: 731e                jae  0xa43fd3           ; want >= B -> reject
00a43fb5: 8b5124              mov  edx,[ecx+0x24]     ; file base
00a43fb8: 2bd6                sub  edx,esi            ;  - A_lo
00a43fba: 8b742410            mov  esi,[esp+0x10]     ; arg0 = out0
00a43fbe: 03d0                add  edx,eax            ;  + want_lo
00a43fc0: 8916                mov  [esi],edx          ; *out0 = file offset
00a43fc2: 8b4910              mov  ecx,[ecx+0x10]     ; B_lo
00a43fc5: 8b542414            mov  edx,[esp+0x14]     ; arg1 = out1
00a43fc9: 5f                  pop  edi
00a43fca: 2bc8                sub  ecx,eax            ;  - want_lo
00a43fcc: 5e                  pop  esi
00a43fcd: 890a                mov  [edx],ecx          ; *out1 = bytes left
00a43fcf: 5b                  pop  ebx
00a43fd0: c21000              ret  0x10
00a43fd3: 8b442414            mov  eax,[esp+0x14]     ; arg1
00a43fd7: 5f                  pop  edi
00a43fd8: 5e                  pop  esi
00a43fd9: c70000000000        mov  dword ptr [eax],0  ; *out1 = 0
00a43fdf: 5b                  pop  ebx
00a43fe0: c21000              ret  0x10
00a43fe3: cc                  int3
```

`0xa43fb5` **is** an instruction boundary: the `jae` at `0xa43fb3` is two bytes
and ends exactly there; it is also a Jcc target (`0xa43faf: jb 0xa43fb5`,
`0xa43fb1..b3` falls through) and the first block of the accept path. Capstone
linear disassembly from `0xa43f80` stays in sync through `ret` and the `int3`
padding, so no mid-instruction leader is involved.

### Contract

`bool translate(this, uint32_t *out_file_off, uint32_t *out_len,
                uint32_t want_lo, uint32_t want_hi)`

with the object (`ecx`) holding two little-endian 64-bit pairs and a base:

| field  | meaning                                    |
|--------|--------------------------------------------|
| `+0x08`| window begin lo, `+0x0c` begin hi (A)      |
| `+0x10`| window end lo,   `+0x14` end hi   (B)      |
| `+0x18`| current stream position lo/hi (written elsewhere) |
| `+0x20`| file/window base lo (used by the caller)   |
| `+0x24`| base offset added to the hit               |
| `+0x28`| flags (bit 0 set by the caller on entry)   |

Accept iff `A <= want < B` (64-bit); then

* `*out_file_off = [this+0x24] + (want_lo - A_lo)`
* `*out_len     = B_lo - want_lo`  (bytes remaining in the buffered window)

Reject otherwise and write `*out_len = 0`.

### Callers

Byte-scan of the image for `E8/E9 rel32` to `0xa43f80`: `0xa4426a`, `0xa4440b`
(stream seek/advance paths) and the two inside the buffered reader
`0xac6c10` — `0xac6bc7` and `0xac6d42`. In `0xac6c10` the call is
`translate(this, out, out_len, offset_lo, offset_hi)` on the "serve from the
already-buffered window" branch; the caller then either copies from the window
or refills. `0xac6c10` is the function the menu retry loop re-enters
(`docs/PORT_LOG.md` rows 236/250–252), so this leaf is *on the retry path*, but
it is not itself a loop.

### What it is not

No array walk, no shader registration, no job check, no lock, no readiness
poll. It cannot spin or deadlock on its own: one pass, four comparisons, two
stores. A hot `0xa43fb5` therefore means the *caller* is re-issuing window hits,
not that this function waits.

## 2. Instrumentation (`TESV_A43F_TRACE=1`, off by default)

`source/n3ds_lift_runtime.c`:

* `n3ds_a43f_trace_dispatch()` — called from `n3ds_rt_dispatch` for every tier.
  Counts entries to the reader `0xac6c10` (with the requested byte count from
  `[esp+8]`) and to its raw read `0xac6380`, and keeps `g_stream_obj`.
* AOT tier: `n3ds_a43f_trace_aot_pre/post` wrap the generated body. Because the
  body keeps `esp` on the guest stack, `*out1` is the dword at `esp+4` after
  `ret 0x10`: `0` = reject block `0xa43fd3`, non-zero = accept block path
  `0xa43fb5`. Exact, no code fetch.
* Forcing the hook past the AOT inline call cache. Generated bodies call a
  known AOT callee directly (`e_->fn(c)` from a per-site cached `FnEntry`), so
  `n3ds_rt_dispatch` is **not** called on repeat calls. While the env is set,
  `n3ds_rt_dispatch` therefore routes `0xa43f80`, `0xac6c10` and `0xac6380`
  through `container_dispatch()` before the normal AOT lookup. The container
  predicate sends `0xa43f80` to its exact IR record (block hooks fire) and the
  two reader addresses stay on their AOT bodies (dispatch still counts them).
  This is what makes every call visible; without it the counters stay at zero
  even when the guest is inside the function.
* IR/IRC tiers: `n3ds_a43f_trace_pre()` runs before `exec_block` (true entry
  registers/TEB/args, repeat detector, sampled dump) and
  `n3ds_a43f_trace_block()` after it (all 11 block leaders plus the
  accept/reject edges). `n3ds_ir_run` and `irc_run` call both.
* Sampling: visits 1–4, then 8, 16, 32 … print TEB, guest registers, all five
  object fields and four stack arguments *with effective addresses*, the
  computed `want`, the exact Jcc decision (`ACCEPT`/`REJECT` + which of the
  three 64-bit checks failed) and the predicted `*out0/*out1`. Flags are never
  re-read from the guest; the decision is recomputed from the already loaded
  fields.
* Every 1 s: per-tier visits / accept / reject / current & maximum run of
  identical `(this, want)` pairs, plus reader calls, requested bytes, raw-read
  count and the stream object window/position. Two equal windows can therefore
  be compared directly.

`n3ds_irc_*` is untouched; the hook only reads the last block/function the
container already publishes.

`stream_trace` (`TESV_STREAM_TRACE`) is fixed: the entry counter and
`g_stream_obj` now advance on **every** call (the print itself stays capped at
12 lines), and the header prints `entry#N` so a suppressed tail is visible.

## 3. Run status — blocked by a lost generated table

The image and the IRC are intact, but the full AOT table used by the earlier
census (`E:\Skyrim_N3DS_build\analysis\growth_probe\gen01`, `tesv_12.c` 24.6 MB,
`f_A43F80` at table line 29703) **disappeared from the drive during this
session**: `E:\Skyrim_N3DS_build\analysis` now lists as an empty directory and
`dir` on it hangs, while its previous contents are gone from every backup path
searched. The nearest complete alternative, `E:\Skyrim_N3DS_bsa\gen_trace`
(63,944 AOT entries, `f_A43F80` present), is an older discovery set and does
**not** contain `0xa4b4a0`.

Instrumented build and runs (160 MiB, `memprobe`, IRC
`E:\census_ir2\ir_container\tesv.irc`, `sIntroSequence=`):

| run | steps | env | result |
|-----|-------|-----|--------|
| A | 2e8 | all menu probes + `TESV_A43F_TRACE=1 TESV_STREAM_TRACE=1` | `Guest fault at 00000008`, `last_call=00ac6400 irc_fn=00a4b4a0 irc_block=00a4b4a9`; `[a43f]` counters all 0, reader `0xac6c10` never entered |
| B | 2e7 | probes only | `pilot basic-block budget exceeded` |
| C | 2e8 | probes only (no trace env) | same `00000008` fault, same `last_call/irc_fn` |
| E | 2e8 | probes + `TESV_A43F_TRACE=1` (forced-dispatch path) | same fault at t=1.04 s; `vis=0 reader reads=0` |

Run C proves the fault is not caused by the instrumentation: it is the
`gen_trace` slice lacking `0xa4b4a0` (it then runs from the container with a
NULL object at `+8`). The menu reader is never reached, so no `0xa43f80` visit
exists to count in this tree, and the two 10–15 s windows could not be taken.

Default behaviour is unchanged: with `TESV_A43F_TRACE` and
`TESV_STREAM_TRACE` unset all hooks return after one cached-flag test, the
container AOT predicate is unchanged, and run C is byte-for-byte the previous
default path.

## 4. What remains

1. Restore a complete AOT table (regenerate `gen01`, or copy the table that
   produced `E:\census_ir2\build2\run_tesv_slice_36672\tesv_slice.exe`).
2. Re-run A on it; the periodic lines then give the two windows. Expected
   reading: identical `(this, want)` with a non-zero `rep` run and constant
   `*out1` while the retry loop's status stays 0 means a retry/livelock on the
   same window hit (`ACCEPT`), not a deadlock inside this leaf.
3. Compare `TESV_D3D9_SHADER_USAGE=1` on/off on that table. Without it the
   timing shifts, but the counters above are additive and comparable.
