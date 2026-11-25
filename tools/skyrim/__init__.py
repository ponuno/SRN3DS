"""Read-only analysis and conversion prototypes for Skyrim 2011 assets.

This package only *reads* the game installation.  Nothing here writes to the
game directory, and no derived game bytes are stored in the repository: derived
output belongs under ``E:\\Skyrim_N3DS_build\\assets`` (see ``docs/ASSETS.md``).

Modules
-------
bsa      Bethesda BSA v104/v105 archive reader (deflate, embedded names).
dds      DDS header parser and BC1/BC2/BC3/BC4/BC5 -> RGBA8 decoders.
convert  DDS -> PNG and the documented tex3ds ETC1 hand-off.
etc1     Bounded-memory DDS/BC -> ETC1/ETC1A4 pipeline, SKTX container and
         preview-PSNR quality measurement.
nif      Skyrim NIF header/block-table parser and geometry statistics.
hkx      Havok packfile detection/classification (not a full reader).
esm      Minimal TES4/ESM record walker used for real per-cell file lists.
budget   Streaming/LRU models for VRAM and linear-heap working sets.
shaders  Skyrim .fxp shader-package inventory and D3D9 bytecode statistics.
"""

__all__ = ["bsa", "dds", "convert", "etc1", "nif", "hkx", "esm", "budget", "shaders"]
__version__ = "0.1.0"
