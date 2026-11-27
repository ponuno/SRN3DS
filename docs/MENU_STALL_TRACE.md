# MENU_STALL_TRACE — why the retail StartMenu sends no geometry

Status: **host-slice measurement; no frame claimed.** Worktree `menu-gfx`,
slice `E:\Skyrim_N3DS_build\live_arm_check2\gen` + D3DX pilot gen, IRC
`E:\census_ir2\ir_container\tesv.irc`, root `F:\Skyrim` with the two profile
INIs and `sIntroSequence=` / `bPreloadIntroSequence=0`.

## What the run proves

Env: `TESV_MENU_TRACE=1 TESV_DRAW_PROBE=1 TESV_FILE_TRACE=1` plus the
measurement-only renderer probes. The run reaches `Present` and idles.

| Observation | Value |
|---|---|
| `Menu::CreateMovie` `0xa60c80` | entered twice (`from=0x86f067`, `0x85b094`) |
| `Menu::movieName` `0xa60c10` | entered twice |
| `0xa5eaa0` name resolve | entered twice (`a0=0x1007fb0c/0x1007fb1c`) |
| `0xa5ed20` BSScaleformFileOpener | **never entered** |
| `0x8779d0` message switch | **never entered** → no message, no type 0 |
| `0x877a50` GFx FadeInMenu | **never entered** |
| `0x8765f0` fadeOutStarted | **never entered** |
| `[0x1b3fc98]` menu singleton | `0` at every ctor/CreateMovie entry |
| `[0x1b3fc94]` flag | `0` |
| Wrapper `0xf874c0` `arg0` | `0x400` → alpha byte `0x00` on every frame |
| `Draw*` | `0` without a clock offset |
| Thread 12 | `block ... ms=4294967295 ... caller=0xa4e30f h=537(sem)` after `release h=538 ... 0xa4e2ee` |

The SWF payload is never read. `Skyrim - Interface.bsa` is opened and the engine
reads `interface\fonts_en.swf` (33470867..33878676) and
`interface\fontconfig.txt` (34634916..34636010), then the readahead window stops
at 33947648 — 35,703 bytes into `interface\startmenu.swf`
(33911945..33972129). The `Update.bsa` copy at 16978297 is likewise untouched:
the last Update read ends at 16973824.

So this is **not** a missing mount and not a case-insensitive path problem:
`Interface.bsa` resolves and serves other members. The menu movie load is
enqueued and the worker thread then blocks forever on semaphore `0x537`; the
main thread sleeps, the message switch never runs, and the only submit is the
engine's own transparent 2D quad (`arg0=0x400`, alpha 0). No GFx mesh batch is
ever built because no movie def exists.

## Time-offset experiment

`TESV_TIME_OFFSET_MS=30000` (clock shifted by 30 s at `win32_init`) changes the
frame: two `DrawIndexedPrimitive` calls appear (`caller=0xcd39fa`,
`a1=0x50002020`, `a2=4`). They are **not** menu geometry:

* the wrapper's `arg0` is still `0x400` (alpha byte 0);
* no `DrawPrimitive(TRIANGLESTRIP, start, 2)` from `0xf87e53`;
* `0x8779d0` / `0x877a50` are still never entered;
* the same semaphore block follows.

So alpha 0 is **not a fade that the clock forgot to finish**; shifting the clock
does not enter the menu state machine. The transparent quad is the normal
pre-menu/loading submit, and the blocker is the stalled movie-load handoff.

## Instrumentation added

* `TESV_FILE_TRACE=1` — every `.bsa/.swf/.gfx` open (guest path, host path,
  caller VA, size) and every read (handle, offset, requested/got bytes).
* `TESV_MENU_TRACE=1` — `0x69de08/0x876f60/0x876fc0/0x876b90/0xa60c80/
  0xa60c10/0xa5eaa0/0xa5ed20/0x8779d0/0x877a50/0x8765f0/0x876bf0/0x876a50/
  0xa64ba0/0xa64940` with `singleton`, `flag94`, args; `0x8779d0` prints the
  message word and type; `0x877a50` prints `[menu+8]` and `[[menu+8]+0x10]`.
* `TESV_TIME_OFFSET_MS` / `TESV_TIME_SCALE` — guest-visible `GetTickCount`,
  `timeGetTime`, `QueryPerformanceCounter` shift/scale (offset logged).
* `TESV_AUTO_ENTER_MS` — one real `VK_RETURN` down/up after N host ms.
* `TESV_DRAW_PROBE` — now also logs every `Draw*` with caller and args, and
  samples the menu singleton/view every 120 wrapper state calls.
* `tools/menu_probe.py LOG [--json OUT]` — one-screen report: presents, draws,
  wrapper alpha gates, file opens/reads, archive ranges, milestones, last
  thread sleep, first fault. `tests/menu_probe_test.py` covers the parser.

## Next concrete step

The semaphore handoff is the first host-side suspect: `0xa4e2ee` releases
`0x538`, then the same worker blocks forever on `0x537` at `0xa4e30f`. A real
second thread would consume the queued movie-load job; under the cooperative
thread model nothing does. Fixing/queueing that handoff (or proving the guest
expects a different wait) is the next measurable step before any GFx draw can
appear.
