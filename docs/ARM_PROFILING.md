# ARM11/Azahar timing profiles for AOT/IR selection

## Scope

`tests/isolated/armtime/` is an isolated benchmark for the **time-profile format
and measurement methodology**. It executes a small deterministic x86 corpus
through both the real generated AOT fixture and the real IR decoder/interpreter
on ARM11 under Azahar, verifies equivalence, and writes selector-compatible CSV.

It is **not** a Skyrim/full-game profile and it is **not** New 3DS hardware
timing. Every result and manifest labels the run as `azahar_emulator` and sets
`hardware_time_claim: false`. Azahar's configured CPU clock multiplier, JIT, host
load, and timer emulation all affect the run; do not turn these numbers into
device performance claims.

The isolated Makefile compiles existing runtime/fixture sources as read-only
dependencies. It is not referenced by the main Makefile and does not modify main
runtime or build inputs.

The archived Azahar log records `Core_CPUClockPercentage: 400`,
`System_IsNew3ds: true`, and an unimplemented `ConfigureNew3DSCPU` service. Those
are emulator configuration/service facts, not evidence of hardware behavior.

## Files

| Path | Role |
|---|---|
| `tests/isolated/armtime/main.c` | ARM11 harness, verification gate, timers, CSV/JSON writers |
| `tests/isolated/armtime/Makefile` | isolated ARM11 `.3dsx` build |
| `tests/isolated/armtime/fixture.json` | expected addresses, extents, workloads, hit counts, exclusions |
| `tests/isolated/armtime/run_azahar.ps1` | bounded Azahar run and evidence collection |
| `tests/isolated/armtime/test_arm_profile.py` | host-only tool tests |
| `tools/arm_profile.py` | bounded archive collector and verified selector check |
| `tests/isolated/armtime/results/` | archived runs, including the successful fixture run |

Runtime/fixture dependencies are `source/ir_*.c`, `source/n3ds_lift_runtime.c`,
`source/guest.c`, `staged/fpu.c`, `tests/ir_corpus.h`, and
`tests/ir_aot_fixture.c`.

## Build, run, and check

From the repository root in the devkitPro MSYS2 environment:

```sh
make -C tests/isolated/armtime clean
make -C tests/isolated/armtime
```

Run in desktop Azahar (Windows PowerShell):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tests/isolated/armtime/run_azahar.ps1 -TimeoutSec 90
```

The runner uses the same `Global\SkyrimN3DS_AzaharRun` mutex as the existing
Azahar runner, refuses to share SD/log state with another run, starts and closes
only its own Azahar process, and writes a timestamped directory under
`tests/isolated/armtime/results/`.

Validate the archived CSV and feed it to the time-ranked selector:

```powershell
py -3 tools/arm_profile.py check `
  --profile tests/isolated/armtime/results/<run>/armtime_profile.csv `
  --metadata tests/isolated/armtime/results/<run>/armtime_meta.json `
  --out tests/isolated/armtime/results/<run>/selection --limit 5
```

`check` invokes `tools/ir_select.py --rank-by time --validate`, verifies the
selector's ordering and arithmetic, and writes `selection/check.json`. It reports
two different things explicitly:

1. **Selector AOT/IR partition:** the share of recorded profile time covered by
   the selected AOT functions; the complement is assigned to IR.
2. **Measured tier totals:** AOT warm versus IR warm execution time. This is a
   tier comparison, not the selector partition.

Host-only tests (no emulator required):

```sh
py -3 tests/isolated/armtime/test_arm_profile.py
```

## Measurement contract

### Clock and CSV unit

The guest uses `svcGetSystemTick()`. The harness records the emulated system tick
rate as **268,111,856 Hz** and emits ticks directly:

```csv
addr,hits,ticks
00501000,4096,3403871
```

`tools/ir_select.py` accepts `ticks` as a complete measured-time column. Using
ticks avoids presenting a host-wall-clock or uncalibrated nanosecond value as a
hardware measurement. A tick-to-nanosecond conversion, if ever needed, must carry
the same emulator-only label.

`hits` is the number of warm invocations in that case. `ticks` is the sum of all
timed warm invocations, so ranking by time measures aggregate contribution, not
just per-call cost. The canonical `armtime_profile.csv` uses **AOT warm** time as
the baseline execution profile. Diagnostic files are also emitted:

- `armtime_aot.csv`: same AOT warm data as the canonical profile;
- `armtime_ir_warm.csv`: IR warm data with the same hit counts;
- `armtime_ir_cold.csv`: `hits = cold_reps`, time includes decode plus execute.

Each fixture function appears once, keeping addresses unique as required by
`ir_select.py`. Workload labels (`integer`, `memory`, `control`, `x87`, `sse`,
`sustained`) and per-case hit counts are recorded in `armtime_meta.json`.

### Timed boundaries

For every invocation:

1. Shared deterministic guest state is filled **outside** the timed region.
2. `t0 = svcGetSystemTick()`.
3. `n3ds_lift_init_cpu()` and the selected tier invocation execute.
4. `elapsed = svcGetSystemTick() - t0`.

Thus the interval includes CPU initialization, dispatch/invocation setup, and
guest execution. It excludes fixture state generation, result snapshots, file
I/O, and cache-reset work. The timer reads bound the interval; their own SVC cost
is not separately subtracted. This overhead is especially visible for very small
functions, which is why the fixture uses multiple invocations per case.

### Warm versus cold IR

- **AOT warm:** generated code is already present; preflight invocations also
  warm Azahar's translation, and there is no decode phase.
- **IR warm:** the case is invoked once before timing to populate the decode
  cache. The timed loop must observe cache hits and zero cache misses.
- **IR cold:** `n3ds_ir_cache_reset()` runs before each single invocation and is
  not timed. The timed invocation includes decode plus execution. Sixteen cold
  samples are taken per case.
- `decode_est_ticks` is
  `ir_cold_ticks - floor(ir_warm_ticks / hits) * cold_reps`, clamped at zero. It is
  an estimate of decode cost across the cold samples, not a standalone decoder
  timer.

The host check rejects a run unless warm cache misses are zero, warm hits are
present, and cold cache misses are present.

### Verification gate

Before any timing is emitted, each case runs through AOT and IR from identical
state on two deterministic seeds. The harness compares:

- invocation status and return value;
- fault text on failure;
- the complete `CPU` structure;
- all 28 KiB of fixture guest data (`input`, `output`, scratch, and matrices).

After all timed loops, every case is checked again on the second seed. Any
mismatch, trap, missing cache evidence, or file-write error removes the CSV files
and leaves only a FAIL log. The host `check` independently requires both
`verification: pass` and `postflight_verification: pass` before ranking.

The initial state deliberately constrains the corpus's `arith` LOOP count to one
and gives `increment` a valid data pointer. Unconstrained random loop counts or
trap pointers measure a hang/fault, not a useful workload.

### Fixture exclusions

`fixture.json` records why these corpus entries are not timed:

- `cmc`: known AOT flag-lowering divergence documented by `tests/ir_equiv.c`;
- `pilotcall`: needs the separate pilot AOT/import fixture;
- `direct` / `indirect`: an IR top-level call dispatches its callee through the
  AOT table, so the sample is not a pure single-tier measurement.

Trap/unsupported-opcode cases are correctness tests, not timing workloads.

## Archived fixture result

The successful run is
`tests/isolated/armtime/results/20261124-172408-armtime/` (Azahar 2126.2,
`HEAD-219d743`). It contains the `.3dsx`, guest CSVs, metadata, bounded emulator
log tail, runner log, archive hashes, and selector output.

That run produced:

- 15 verified functions, 40,448 warm hits;
- 38,655,823 AOT warm ticks;
- 233,925,703 IR warm ticks (6.05x AOT for this synthetic corpus only);
- top five functions by AOT warm time cover **52.19%** of recorded time;
- selector split: **52.19% AOT / 47.81% IR**.

These numbers validate the CSV format, verification gate, cold/warm separation,
and time-ranked selector integration. They do not predict Skyrim or hardware
behavior. The selector check uses synthetic one-extent function ranges and
`--insn-per-byte 1`; size/closure-projection fields in its manifest are format
artifacts, not measured game-size results. The relevant fixture result is the
measured-time ranking and share.

## Bounded archives

`tools/arm_profile.py collect` copies only the known guest result files. CSV and
JSON are never silently truncated: a result larger than `--max-result-bytes`
(default 256 KiB) is an error. The Azahar log is reduced to a byte-bounded tail
(default 128 KiB) with an explicit truncation marker. `archive_manifest.json`
records original/archived sizes, truncation, and SHA-256 for every file.

The PowerShell runner additionally records the Azahar version, PID, source
`.3dsx` hash, guest marker, and elapsed wall time. It waits for `PASS`/`FAIL` or a
bounded timeout, then closes only the process it started.

## Replacing the fixture with real lifted hot functions

The fixture is intentionally small. To produce a real profile later:

1. **Start from real lifted code.** Replace the corpus/AOT table dependency with
   the production generated `FnEntry` table and the matching original guest
   bytes. Use real `functions.json` basic-block ranges, not the synthetic
   one-extent-per-function map used by this format check.
2. **Provide a real driver or captured state.** Replace `fill_state()` and the
   fixed call schedule with the actual game/workload driver, or deterministic
   captured register/memory states for representative hot functions. Preserve
   the same state-reset rules for AOT and IR.
3. **Keep tier boundaries honest.** Time the baseline tier that actually ran.
   Do not mix cold decode time into the hot selector profile. If a function can
   call imports or cross tiers, account for the callee tier or exclude the case
   from pure-tier comparisons.
4. **Retain equivalence checks.** Compare return value, full CPU/FPU/XMM state,
   and all guest writes before reporting time. Add nested-call cases separately
   if mixed-tier behavior matters.
5. **Record real hits.** Increment each function's hit count at entry in the real
   workload. If a function is measured under several workloads, aggregate its
   hits and ticks into one CSV row; duplicate addresses are rejected by
   `ir_select.py`.
6. **Generalize the host check.** `tools/arm_profile.py check` is deliberately
   strict against `fixture.json` so synthetic data cannot masquerade as a game
   profile. Add a real-profile mode that validates against production
   `functions.json` and runtime verification records instead of the fixture
   address/hit table.
7. **Run the selector on the real CSV.** Use
   `python tools/ir_select.py REAL.csv REAL_functions.json --out OUT --rank-by time
   --validate` with an explicit AOT budget/limit. Keep the emulator/hardware label
   in the profile metadata and manifest.

Until those steps are done, the archived fixture result should be cited only as
methodology validation, never as a full-game timing profile.
