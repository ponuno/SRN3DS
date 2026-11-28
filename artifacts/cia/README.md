# Skyrim N3DS — CIA artifacts

These are console installers for the New 3DS port. They are built from the
commit named in each section; every byte count and SHA-256 below was taken from
the file in this directory.

## `Skyrim_N3DS_tesv_96x24_memgate.cia` — install this one

- built from `main` `180d102`
- size: **1 580 992 B**
- SHA-256: `d4ceddc4fc6e538a11f75f823855677ef2fa94e277940d0ec6de1ae09e1f683e`
- title id `000400000F0D5900`, 178 MB / 804 MHz / L2 on
- profile: newlib **96 MiB**, linear **24 MiB**, guest budget 92 MiB
- ELF symbols: `__ctru_heap_size=0x06000000`,
  `__ctru_linear_heap_size=0x01800000`, `__stacksize__=0x00020000`

Install on the SD card with the usual title installer (the file is the CIA
itself). Required SD layout and configuration keys are in
[`docs/CIA_INSTALL.md`](../../docs/CIA_INSTALL.md).

Fixes included:

1. **Hardware heap limit.** libctru's `OS_HEAP_AREA` is
   `0x08000000..0x0E000000` (96 MiB). A `__ctru_heap_size` of 120 MiB makes
   `svcControlMemory` fail in `__system_allocateHeaps` and the console aborts
   with `prefetch abort (svcBreak)` before `main`. The 96x24 profile is the
   largest that boots.
2. **Startup minimum-config check.** Skyrim calls `GlobalMemoryStatusEx` from
   `0x00a48a5d` and requires a 512 MiB commit limit (RAM + page file). The port
   answers **only that call** with the PC-shaped value; every other query stays
   honest and the 92 MiB guest budget is still enforced. No `TESV_MEMPROBE` is
   needed. Look for
   `[mem] startup minimum-config check: 512 MiB commit reported ...`.
3. **Bink empty clip.** On the 3DS there is no Bink decoder, so `BinkOpen`
   returns a valid empty clip (0x0, one frame, no-op copies) instead of `NULL`;
   the intro state can finish and `StartMenu` is created. Look for
   `[bink] ... empty clip` before `[menu] enter 00876f60`.

The global D3D9 capability report is unchanged (`DevCaps = 0x00082000`); no
pixels or sizes are fabricated by the empty clip (nothing reaches the
framebuffer).

## `Skyrim_N3DS.cfg` — device runtime settings

Copy [`Skyrim_N3DS.cfg`](Skyrim_N3DS.cfg) to `sdmc:/Skyrim_N3DS.cfg`. It holds
the 96x24 VRAM/texture/paging limits from `docs/CIA_INSTALL.md` §5; the file is
read once at launch, so editing it needs no rebuild. `TESV_MEMPROBE` is
measurement-only and must stay commented out; the startup check is handled by
the targeted shim above.

## Removed installers

- `Skyrim_N3DS_tesv_96x24_menu.cia`: booted but **exits at the startup memory
  check** (`exit code=0 from=00f5894a`); replaced by the memgate build.
- `Skyrim_N3DS_tesv_120x24_menu.cia` and `..._120x24_stack.cia`: **abort before
  `main`** because a 120 MiB heap does not fit the 96 MiB libctru heap area.
  Do not reinstall them from history.
