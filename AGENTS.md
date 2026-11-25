# Agent working rules

## Parallelism and throughput

- This workstation has **16 logical CPUs**. Every new subagent task must
  explicitly optimise independent work for up to 16 threads and report wall
  time, not just CPU time.
- Prefer process-level parallelism for independent files/functions:
  - host C compilation: `HOST_CLANG_JOBS=6` (up to 8 on this machine);
  - future lift/instrument/slice shards: parallel processes with deterministic
    file partitioning and byte-identical output;
  - independent benchmarks/tests may run concurrently when they use distinct
    work dirs and bounded memory.
- Do not spawn more workers than logical CPUs. Large generated C files can use
  ~1–2 GiB each; keep total compile concurrency bounded and monitor memory.
- Measure before/after wall time for any throughput change. A single-core
  process on a 16-thread host is a bug in the harness, not a CPU shortage.

## Execution safety

- Do not stop or modify a live growth/build/supervisor run to apply an
  optimisation. Prepare changes in new files or isolated dirs, test them, then
  land at a safe stage boundary.
- All ARM builds use `tools/build.ps1 -Isolated <name>`; never overwrite shared
  artifacts.
- Intermediates stay on `C:`/`E:`. The console SD card (`F:`) gets only final
  diagnostic `.3dsx`/CIA files and owner-provided game inputs.
- Subagents run on `opencode-go/deepseek-v4.1-flash` unless the user explicitly
  requests another model.

## Honesty rules

- Unknown functions/imports, unsupported IR opcodes, shader features and memory
  states must fail explicitly with a named diagnostic.
- Never claim a frame, hardware run, parity result, compression ratio or speedup
  that was not measured and archived.
- Distinguish host, Azahar and physical New 3DS evidence.
