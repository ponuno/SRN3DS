# Parallel TESV lift — landed implementation and benchmark

## Landing status

Landed in the live tree:

- `staged/lift.py` — `--jobs=N` / `HOST_LIFT_JOBS`, default 1 (serial).
- `E:\Skyrim_N3DS_build\grow16_manual.ps1` — sets `HOST_LIFT_JOBS=8` when unset.

Verified after landing on a 60-function slice: `--jobs=1` and `--jobs=4`
produced 11 identical files. The authoritative differential remains the
scratch benchmark on `growth26/iter01` (42,964 functions): 27 files,
217,184,203 bytes, byte-identical at 1/4/6/8 jobs.

Parallel mode is only worth it above roughly 5k functions; below that the
per-worker PE/JSON load dominates. `grow16_manual.ps1` always lifts 42k+.

Original scratch report follows.


Scratch tree: `E:\Skyrim_N3DS_lift_parallel` only. Nothing in
`C:\Users\vasya\Documents\Skyrim_N3DS`, in the live growth scripts, or in
`E:\Skyrim_N3DS_build\analysis\g*` was modified.

## 1. What is here

| Path | Purpose |
| --- | --- |
| `staged/lift.py` | Copy of the live lifter + `--jobs=N` / `HOST_LIFT_JOBS` shard mode and a shared `write_outputs()` |
| `staged/peimg.py`, `staged/lift_prelude.h`, `staged/rt_table.h`, `staged/x86.h` | Minimum lifter inputs (`peimg.py` is imported; the headers are for reference) |
| `tools/instrument_generated.py` | Copy of live, unmodified |
| `tools/instrument_generated_parallel.py` | Per-file parallel instrumentation (evaluation only) |
| `bench/run_bench.py` | Wall time, peak process-tree RSS, CPU seconds/utilisation |
| `bench/hash_tree.py` | Byte-for-byte directory comparison |
| `bench/bench_lift.ps1` | Driver: jobs 1/4/6/8, hash check, summary JSON |
| `bench/g26_ref/` | Serial reference output for growth26/iter01 (27 files, 217,184,203 bytes) |
| `bench/hw_j1/` | Serial reference output for hwnext2 (27 files, 605,163 bytes) |
| `patch/lift_py.patch` | Exact later patch for `staged/lift.py` |
| `patch/grow16_manual.patch` | Exact later patch for `E:\Skyrim_N3DS_build\grow16_manual.ps1` |
| `patch/make_patches.py` | Regenerates both diffs from `patch/*_live.*` |

## 2. Semantics: why sharding is byte-identical

The serial `main()` is:

```
all_funcs = set(funcs)
for rnd in range(3):
    for fa in sorted(funcs):          # skip already failed
        lift fa with (all_funcs, funcs, summaries)
        good[fa] or failed[fa]
    if set(good) == all_funcs: break
    all_funcs = set(good)
write funcs_MOD.h, MOD_NN.c, MOD_table.c, MOD_rejected.json
```

`Lifter(fa, blocks, all_funcs, funcs, summaries)` is the only thing that
produces C text, and it reads no mutable global state. Therefore, for one
round, every function is a pure function of
`(fa, blocks, all_funcs, funcs, summaries)`. Sharding a round cannot change
any function's result.

The one cross-function dependency is the round barrier: `all_funcs` for round
`r+1` is the accepted set of round `r`. The parallel version keeps the same
three rounds and consumes the whole round (`executor.map`) before computing
the next `all_funcs`. It never reuses a stale known set.

Ordering details that matter for bytes:

* `funcs_MOD.h`, buckets and `MOD_table.c` are written from `sorted(good)`.
* `MOD_rejected.json` is dumped in `failed` insertion order. The serial loop
  inserts failures in ascending address order, round by round. The parallel
  merge therefore sorts each round's failures before adding them to `failed`.
  (`good` insertion order is irrelevant; only its keys/lines are used.)
* Output writing is one shared function used by both paths, so jobs=1 and
  jobs=N cannot drift in formatting.
* Bucketing is computed from the global address range after merging; shard
  boundaries never influence which bucket a function lands in.

Failure handling: only `Unsupported` is caught, exactly as serial. Any other
worker exception, or a dead worker (`ProcessPoolExecutor` raises
`BrokenProcessPool`), aborts the run; output is only written after all rounds
complete, so there is no partial/divergent output to mistake for success.

### Round behaviour measured on growth26/iter01

Stderr with `--jobs>1` reports each round:

```
lift: round 1: 42964 pending, 42962 accepted, 2 rejected (+2), known 42964 -> 42962
lift: round 2: 42962 pending, 42962 accepted, 2 rejected (+0), known 42962 -> 42962
```

Two functions (`0x636573 junk aas`, `0x676f46 junk daa`) are rejected in
round 1. Round 2 re-lifts the remaining 42,962 against the shrunken known set
and changes nothing, so the loop stops after round 2.

A deliberately modified copy with `range(3)` → `range(1)` also produced
byte-identical output on this slice, i.e. the known-set refresh happens to be
a no-op here. The shipped version still uses per-round barriers, so it stays
serial-equivalent on slices where a rejected callee *does* change a caller's
spill/reload text. Do not replace the barriers with a one-round shortcut
based on this single slice.

## 3. Usage

```powershell
$py = 'C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe'
& $py E:\Skyrim_N3DS_lift_parallel\staged\lift.py `
    E:\skyr\TESV.exe `
    E:\Skyrim_N3DS_build\analysis\growth26\iter01\slice_functions.json `
    E:\Skyrim_N3DS_lift_parallel\bench\out tesv 400000 --jobs=8
```

or set `HOST_LIFT_JOBS=8` (`LIFT_JOBS` is also accepted; `--jobs=` wins).
Default is jobs=1, i.e. the original serial path, so an accidental direct run
never changes behaviour. Values are clamped to 16.

The grow16 patch only adds the default:

```powershell
if(-not $env:HOST_LIFT_JOBS){ $env:HOST_LIFT_JOBS = '8' }
```

so the existing `& $python staged/lift.py ...` line keeps working unchanged.

## 4. Benchmarks

Hardware: AMD Ryzen 7 7700 (8 cores / 16 logical), 32 GB RAM, Windows.
Python 3.13.3, capstone 5.0.7. Both slices are TESV.exe rebased to 0x400000,
`MOD=tesv` (24 buckets).

Method: fresh output directory per run (so bucket writes are real, not
`write_if_changed` no-ops); wall time measured around the whole process tree;
peak RSS is the maximum sum of working sets of the process tree sampled every
100 ms (shared pages are counted once per process, so this is an upper
bound); CPU is `GetProcessTimes` user+kernel over every process in the tree
divided by wall time.

### 4.1 Large slice — `growth26/iter01/slice_functions.json` (42,964 functions)

42,962 accepted, 2 rejected. Output: 27 files, 217,184,203 bytes.

| jobs | wall s | speedup | peak tree RSS MB | peak worker MB | CPU s | CPU util % (of 16) | cores busy | byte-identical |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| 1 (serial) | 242.3 | 1.00× | 1,277.7 | 1,273.6 | 238.4 | 6.1 | 0.98 | reference |
| 4 | 75.3 | 3.22× | 4,093.2 | 1,283.4 | 274.4 | 22.8 | 3.64 | yes |
| 6 | 61.1 | 3.97× | 5,502.6 | 1,290.9 | 324.4 | 33.2 | 5.31 | yes |
| 8 | 58.5 | 4.14× | 6,918.3 | 1,271.6 | 389.5 | 41.6 | 6.66 | yes |
| 16* | 56.1 | 4.32× | 12,572.7 | 1,277.8 | 495.0 | 55.1 | 8.82 | yes |

`*` 16 was an accidental extra run from the first (broken) harness invocation
and was hash-checked separately; it is not the recommended default.

Two independent serial runs took 237.6 s and 242.3 s (~2 % variance), so the
8-job speedup is real but should be read as ≈4×, not 4.14× exactly.

Observed limits: each worker re-loads `TESV.exe` and the 17 MB
`slice_functions.json` under Windows `spawn`, costing ~1.27 GB RSS and ~5 s
per worker. Growth from 6→8 jobs buys only 4 % wall time for +1.4 GB, and
16 jobs buys another 4 % for +5.7 GB. CPU tops out at ~6.7 of 8 physical
cores because of the parallel load, the single-threaded merge/217 MB IPC, and
the single-threaded bucket write tail.

### 4.2 Small slice — `hwnext2/gen/aot_functions.json` (32 functions)

32 accepted, 0 rejected. Output: 27 files, 605,163 bytes. Wall is the best of
3 runs.

| jobs | wall s | speedup | peak tree RSS MB | CPU s | CPU util % (of 16) | cores busy | byte-identical |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| 1 (serial) | 4.36 | 1.00× | 512.9 | 4.3 | 6.1 | 0.98 | reference |
| 4 | 9.51 | 0.46× | 2,549.6 | 24.7 | 16.2 | 2.60 | yes |
| 6 | 10.50 | 0.42× | 3,529.2 | 40.3 | 24.0 | 3.84 | yes |
| 8 | 11.36 | 0.38× | 4,559.0 | 58.7 | 32.3 | 5.16 | yes |

Parallelism **loses** here: the ~4.4 s fixed cost (PE load, capstone, JSON,
24 bucket writes) is paid once serially but once per worker in parallel. The
same effect is visible at 3,000 functions (`growth4/iter01`, 17.6 MB output):
12.86 s / 11.85 s / 13.09 s for jobs 1 / 4 / 8, all byte-identical. Break-even
is somewhere around a few thousand functions; the growth16 slices are 27k–43k,
where the gain is large.

## 5. Instrumentation evaluation (`tools/instrument_generated.py`)

Workload: the serial g26 reference (217 MB, 24 buckets, 804,280 labels).
`instrument_generated_parallel.py` applies the same per-file transform in a
process pool; files are independent, each is written once, so output does not
depend on scheduling.

| variant | wall s | peak tree RSS MB | CPU util % (of 16) | byte-identical |
| --- | ---: | ---: | ---: | :---: |
| serial (live tool) | 3.70 | 104.8 | 5.9 | reference |
| `--jobs=8` | 1.13 | 356.6 | 31.0 | yes |

The 3.3× is real and the result is identical (27 files, 236,488,075 bytes
after prefixing includes and inserting 804,280 ticks). But in absolute terms
it saves 2.6 s per grow iteration: ~1 % of a 242 s serial lift, or ~4 % of a
58 s 8-job lift. **Verdict: worth including only as an optional flag** (patch
the live tool to accept `--jobs`, or keep using it serially). It is not part
of the main win and should not block the lift patch.

## 6. Verdict

* Default `HOST_LIFT_JOBS=8` for grow16: measured 4.1× on 42,964 functions,
  6.9 GB peak tree RSS on a 32 GB box, byte-identical output.
* Use 4–6 if the box has 16 GB or other work is running; use 1 on slices
  below ~5k functions, where startup dominates.
* Do not raise the default to 16: +4 % wall time over 8 for +5.7 GB.
* Keep the per-round barrier; the one-round equivalence seen on growth26 is
  an empirical observation, not a licence to change serial semantics.

## 7. Later patch (not applied)

Both patches were dry-run applied to copies of the live files; the result was
SHA-256-identical to `staged/lift.py` / `patch/grow16_manual.ps1`.

```powershell
cd C:\Users\vasya\Documents\Skyrim_N3DS
git apply --check E:\Skyrim_N3DS_lift_parallel\patch\lift_py.patch
git apply         E:\Skyrim_N3DS_lift_parallel\patch\lift_py.patch

# grow16_manual.ps1 lives outside the repo:
cd E:\Skyrim_N3DS_build
git apply --check E:\Skyrim_N3DS_lift_parallel\patch\grow16_manual.patch
git apply         E:\Skyrim_N3DS_lift_parallel\patch\grow16_manual.patch
```

The grow16 diff targets `tools/grow16_manual.ps1` only by name; apply it to
`E:\Skyrim_N3DS_build\grow16_manual.ps1` (the file it was generated from).
If `git apply` refuses because of CRLF settings, the whole change is the four
lines shown in section 3.

Before trusting it on a live growth run, lift one slice with `--jobs=1` and
`--jobs=8` into two directories and check:

```powershell
python E:\Skyrim_N3DS_lift_parallel\bench\hash_tree.py DIR_SERIAL DIR_PARALLEL
```

## 8. Caveats

* Windows `spawn` means every worker re-loads the PE and slice JSON and
  re-imports capstone/pefile: ~0.5 GB baseline per worker even for 32
  functions, ~1.27 GB for growth26. Memory scales with jobs × slice, not with
  the amount of work; monitor it on larger slices. An 8-worker run on a slice
  several times bigger than growth26 could approach 32 GB; 4–6 is safer there.
* Peak RSS is the sum of working sets, an upper bound; private bytes are
  lower. Values include shared Python/DLL pages per process.
* Timings are single runs per large configuration (two for serial), on an
  otherwise idle desktop. Expect ±2–5 %.
* The output write and the merge are still single-threaded. If lift time is
  reduced further, writing 24 buckets of a 200 MB+ output becomes a larger
  fraction (observed tail is a few seconds, included in the numbers above).
* The lifter still assumes `LIFT_*` environment variables are identical in
  parent and workers; `spawn` inherits them, so do not change them mid-run.
* Jobs are clamped to 16, but nothing above 8 was measured safe on other
  slices; the 16-run number is included only to show the plateau.
* `instrument_generated_parallel.py` keeps the live tool's fail-fast checks
  ("already instrumented", label count mismatch) and its `--profile` mode is
  implemented identically, but it was benchmarked only in the default mode.

## 9. Reproduce

```powershell
# large slice, jobs 1/4/6/8, fresh dirs, hash check, summary JSON
powershell -File E:\Skyrim_N3DS_lift_parallel\bench\bench_lift.ps1 -Name g26 `
  -Functions E:\Skyrim_N3DS_build\analysis\growth26\iter01\slice_functions.json

# small slice
powershell -File E:\Skyrim_N3DS_lift_parallel\bench\bench_lift.ps1 -Name hw `
  -Functions E:\Skyrim_N3DS_build\hwnext2\gen\aot_functions.json -Repeat 3

# instrumentation
python E:\Skyrim_N3DS_lift_parallel\bench\run_bench.py --label inst_j1 --out inst_j1.json `
  -- python E:\Skyrim_N3DS_lift_parallel\tools\instrument_generated.py COPY_DIR
python E:\Skyrim_N3DS_lift_parallel\bench\run_bench.py --label inst_j8 --out inst_j8.json `
  -- python E:\Skyrim_N3DS_lift_parallel\tools\instrument_generated_parallel.py COPY2_DIR --jobs=8
```
