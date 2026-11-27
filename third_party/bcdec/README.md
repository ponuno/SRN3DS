# bcdec (vendored)

Source: https://github.com/iOrange/bcdec (v0.985), single header
`bcdec.h`, used only for BC7 decoding.  The file is unmodified.

License: dual MIT / Unlicense, see `LICENSE` (alternative A and B).
Attribution: Copyright (c) 2022 Sergii Kudlai.

Used by `source/d3d9_bc.c` (`D9_BC7`).  BC1/2/3/4/5 keep the existing
hand-written decoder so their byte-exact output and tests do not change.
