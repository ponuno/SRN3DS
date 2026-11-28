# Skyrim N3DS — CIA artifacts

These are console installers for the New 3DS port. They are built from the
commit named in each section; every byte count and SHA-256 below was taken from
the file in this directory.

## `Skyrim_N3DS_tesv_96x24_menu.cia` — install this one

- built from `main` `3e35c40` (96 MiB heap limit + Bink empty-clip fix)
- size: **1 580 480 B**
- SHA-256: `a9ede38e3c5e4cb18c57c1321d0f08873bce8d9acb58b0a2d12fc32ebf0b9501`
- title id `000400000F0D5900`, 178 MB / 804 MHz / L2 on
- profile: newlib **96 MiB**, linear **24 MiB**, guest budget 92 MiB
- ELF symbols: `__ctru_heap_size=0x06000000`,
  `__ctru_linear_heap_size=0x01800000`, `__stacksize__=0x00020000`

Install on the SD card with the usual title installer (the file is the CIA
itself). Required SD layout and configuration keys are in
[`docs/CIA_INSTALL.md`](../../docs/CIA_INSTALL.md).

Two fixes are in this build:

1. **Hardware heap limit.** libctru's `OS_HEAP_AREA` is
   `0x08000000..0x0E000000` (96 MiB). A `__ctru_heap_size` of 120 MiB makes
   `svcControlMemory` fail in `__system_allocateHeaps` and the console aborts
   with `prefetch abort (svcBreak)` before `main`. The 96x24 profile is the
   largest that boots.
2. **Bink empty clip.** On the 3DS there is no Bink decoder, so `BinkOpen` now
   returns a valid empty clip (0x0, one frame, no-op copies) instead of `NULL`;
   the intro state can finish and `StartMenu` is created. Look for
   `[bink] ... empty clip` before `[menu] enter 00876f60`.

The global D3D9 capability report is unchanged (`DevCaps = 0x00082000`); no
pixels or sizes are fabricated by the empty clip (nothing reaches the
framebuffer).

## `Skyrim_N3DS.cfg` — device runtime settings

Copy [`Skyrim_N3DS.cfg`](Skyrim_N3DS.cfg) to `sdmc:/Skyrim_N3DS.cfg`. It holds
the 96x24 VRAM/texture/paging limits from `docs/CIA_INSTALL.md` §5; the file is
read once at launch, so editing it needs no rebuild. The swap keys are commented
out because the device build already defaults to compressed swap on (32 MiB
budget, 32 MiB resident target).

## Removed: `Skyrim_N3DS_tesv_120x24_menu.cia`

The previous artifact used a 120 MiB newlib heap. It **crashes on hardware
before `main`** because the libctru application heap area is only 96 MiB, so it
was removed from this directory. Do not reinstall it from history; use the
96x24 file above. Older `Skyrim_N3DS_tesv_120x24_stack.cia` and
`Skyrim_N3DS_tesv_120x24.cia` are also non-booting and are marked
previous/invalid in `docs/CIA_INSTALL.md`.
