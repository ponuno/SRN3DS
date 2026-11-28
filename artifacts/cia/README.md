# Skyrim N3DS — CIA artifacts

These are console installers for the New 3DS port. They are built from the
commit named in each section; every byte count and SHA-256 below was taken from
the file in this directory.

## `Skyrim_N3DS_tesv_120x24_menu.cia`

- built from `main` `b03c33c` (Bink empty-clip fix)
- size: **1 580 480 B**
- SHA-256: `d2c6aa34ef48a76a78b4a1f88f9b61a1e2c8029808e095c04ac403ad9036031c`
- title id `000400000F0D5900`, 178 MB / 804 MHz / L2 on
- profile: newlib 120 MiB, linear 24 MiB, guest 116 MiB

Install on the SD card with the usual title installer (the file is the CIA
itself). Required SD layout and configuration keys are in
[`docs/CIA_INSTALL.md`](../../docs/CIA_INSTALL.md).

What changed versus the previous installer: on the 3DS there is no Bink
decoder, so `BinkOpen` now returns a valid **empty clip** (0x0, one frame,
no-op copies) instead of `NULL`. That lets the intro state finish and
`StartMenu` be created. Look for this line in the log before the menu
checkpoint:

```
[bink] _BinkOpen@8 ...: no Bink decoder on this platform; empty clip
```

The global D3D9 capability report is unchanged (`DevCaps = 0x00082000`); no
pixels or sizes are fabricated by the empty clip (nothing reaches the
framebuffer).

## Older installers

`Skyrim_N3DS_tesv_120x24_stack.cia` and `Skyrim_N3DS_tesv_120x24.cia` are kept
on the build machine only and are marked previous/invalid in
`docs/CIA_INSTALL.md`; do not install them.
