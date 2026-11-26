# Skyrim LE mods on the New 3DS: Data overlay, scanner, offline optimizer

Status: **file/mod layer only.** The game does not boot yet; nothing here
claims a rendered frame. `DevCaps` is untouched. No game asset (ESP, BSA, DDS,
NIF, SWF) is stored in this repository: tests build synthetic fixtures in a
temporary directory, and real-data checks only read `F:\Skyrim\Data`.

This document covers the layer that can be prepared independently of the
renderer: which plugin wins, which archive is mounted, which loose file
overrides it, what a mod costs in VRAM/guest memory, and how to make a light
mod lighter offline.

---

## 1. Data overlay and load order

Implemented in `tools/skyrim/data_overlay.py`.

### Root resolution

Order: `--data` argument → `TESV_DATA_ROOT` → `SKYRIM_DATA` → probes
(`./Data`, `F:\Skyrim\Data`, the default Steam path). An explicit or
environment path that is not a directory or has no `Skyrim.esm` is a named
`DataRootError`; it never silently falls back to a different tree. This is the
hook for the SD-card layout: point `TESV_DATA_ROOT` at the copied
`/3ds/skyrim/Data` and the same code is used on device.

### plugins.txt / loadorder.txt

LE rules implemented:

* `plugins.txt`: `*Name.esp` = active, `Name.esp` = present but inactive;
  `#`/`;` comments, BOM and blank lines are accepted; duplicate names and path
  separators are rejected.
* `loadorder.txt` (Mod Organizer): bare names, optional `*`, comments; defines
  the base order.
* If both exist, `loadorder.txt` is the base, `plugins.txt` supplies the active
  set and appends plugins missing from the order. Final order is a **stable
  topological sort by `MAST` subrecords**, so a master always precedes its
  dependent regardless of file timestamps.
* A master that exists in `Data` but is not starred is auto-activated (masters
  are not optional). A master that is absent raises
  `MissingMasterError(master, plugin)`; an active plugin whose file is absent
  raises `MissingPluginError`.
* A master cycle raises `PluginOrderError` with the full chain.
* **Vanilla path:** with no list, or with a discovered `plugins.txt` that has
  no `*` at all (the Steam-generated "downloaded content" file), the loader
  uses `Skyrim.esm`, `Update.esm`, then other plugins, all active. This is why
  the current installation (`%LOCALAPPDATA%\Skyrim\plugins.txt` lists only
  Dawnguard/Dragonborn/HearthFires without stars) still resolves vanilla
  correctly.

Active plugin headers are parsed (TES4 `HEDR`, `CNAM`, `MAST`/`DATA`,
compressed header supported); the ESP/ESM bytes are not loaded into guest
memory by the overlay.

### Archive mount order and precedence

Mount order is the load order:

1. `Skyrim.esm` → `Skyrim - Animations`, `Interface`, `Meshes`, `Misc`,
   `Shaders`, `Sounds`, `Textures` (canonical, only those present);
2. `Update.esm` → `Update.bsa` **last among official archives**;
3. each active plugin → `<stem>.bsa` then `<stem> - *.bsa` (case-insensitive,
   sorted), which covers the `Textures` split used by most LE mods.

Asset resolution, highest priority first:

```text
loose file under Data           (always wins, any extension)
last mounted BSA that has it    (later plugin / Update wins)
earlier BSA
missing                         -> None, caller decides
```

A BSA that is present but corrupt raises `ArchiveError`; it is never skipped
so an asset cannot quietly disappear. Duplicate mounts of the same archive keep
the later priority, matching TESV.

`DataOverlay.manifest()` returns the resolved order, per-plugin masters, the
archive list with existence, and the discovery note. `validate()` opens every
archive and reports version/compression/file counts.

---

## 2. BSA versions, compression, embedding

| Version | Game | Header | Directory | Support |
|---|---|---|---|---|
| 103 | Oblivion / some converted mod resources | 36 B (`offset` normally 36) | folder/file records + base-name block | **read only** (writer emits v104/v105); XMem/retain flag bits ignored |
| 104 | Skyrim LE | 36 B | same | read + write (`bsa_write.py`) |
| 105 | Skyrim SE | 36 B | same, archive flags may include 0x100 names | read; SE plugins themselves are Tier 3 |

Evidence for 103: a real
`Mopy/templates/Oblivion/ArchiveInvalidationInvalidated!.bsa` has
`version=103, offset=36, flags=0x703, 1 folder, 1 file`; its data begins
directly with `DDS ` even though bit 0x100 is set. Therefore v103 sets
`0x100` as part of the retain-flags group and does **not** embed a name prefix.
The reader (Python and C) now treats `0x100` as embedding only for v104+ and
accepts `offset >= 34` for v103. No v103 archive appeared in the LE `Data`
tree (all 11 shipped archives are v104), so v103 is defensive support for mods
repacked by older tools, not part of the vanilla path.

Compression is archive-level zlib (`flag 0x004`): `[u8 name_len+name]` when
v104 embedding is on, then `u32 unpacked_size`, then the zlib stream. The
writer emits exactly this and the reader round-trips it in tests. Repacked
archives use zero TES4 hash fields: this port indexes names case-insensitively
and never consults hashes. Do not feed a repacked BSA to the Creation Kit or a
mod manager that validates hashes.

**Loose override** is exercised for `textures/x.dds` over `B.bsa` over
`A.bsa` over `Skyrim - Textures.bsa`, and for `Update.bsa` over the base
archive (`Interface/StartMenu.swf` resolves to `Update.bsa` on the real
installation).

---

## 3. Compatibility matrix

Tier 1 = the existing loader/pipeline can consume it after normal conversion.
Tier 2 = needs an offline pass (already partly implemented). Tier 3 = not
supported and not planned as a shim.

| Tier | Mod shape | What is present | What is missing / action |
|---|---|---|---|
| **1** | ESP/ESM edits, vanilla assets, DXT1/3/5 ≤1024, moderate size, no SKSE | TES4 header/master parse; plugins.txt/toposort; BSA v104 read; loose override; DDS decode; NIF header/geometry stats; ETC1/BC1 writer | Runtime cell/record application, collision, AI; but the file layer is ready |
| **1** | Performance plugins that only delete/move references, occlusion planes, grass density, no scripts | Plugin scan classifies them Tier 1 (`performance_hints`); no runtime scripts needed | Masters must exist (DLC plugins are listed in the local `plugins.txt` but the DLC files are absent from this `Data`) |
| **2** | Optimized/low-res texture packs, 2K→256 downscale, BC7/uncompressed source, custom mip policy | `mod_optimize.py textures`: box downscale, mip chain to 8×8, ETC1/ETC1A4 SKTX, BC1 DDS, source untouched | Colour-space/quality tuning, normal-map re-encoding policy, full pack inventory |
| **2** | Mesh optimization / decimation / LOD stripping | `nif.py` geometry stats; `mod_optimize.py mesh-plan` reports candidates and refuses silent edits | A validated vertex/index rewrite + visual/collision check. Not automatic |
| **2** | Custom SWF / SkyUI / MCM / UI extensions | SWF container + GFx display/text path exists for the menu; source bytes resolve from BSA | General AVM2/SkyUI compatibility, MCM registration, ActionScript API breadth |
| **2** | Script-heavy plugins (≥100 `VMAD`, ≥50 `SCPT`, >8 MiB) | Scanner flags Tier 2 and counts markers | Papyrus VM, script event dispatch, save state. None of it is implemented |
| **2** | Alternate Start / quest mods | Plugin + scripts + custom assets are readable | Scripted start quest, aliases, sequences: blocked on Papyrus |
| **3** | SKSE DLL, SKSE-dependent mods (SkyUI MCM, PapyrusUtil, JContainers, RaceMenu) | Detection via `skse`/`.dll` markers and plugin scan | **No DLL loader, no SKSE API, no PE injection into the guest.** Do not promise it |
| **3** | ENB, ReShade, d3d9.dll wrappers | Detection only | No external d3d9.dll chain, no post-process stack |
| **3** | HDT/SMP physics, Nemesis/FNIS, behaviour generators | Detection only | No Havok behaviour runtime, no hkx animation graph rewrite; `hkx.py` is classification only |
| **3** | ESL/light masters and SE plugins | Detected by flag 0x200 and form version ≥43 | SE record/form layout and ESL form-ID space are not implemented |
| **3** | 4K texture packs | Scanner counts them; optimizer can downscale | Not usable as-is: 4 MiB texture budget and 6 MiB VRAM |

The scanner's tier is the maximum over active plugins: any Tier 3 signal makes
the whole profile Tier 3. Official masters (`Skyrim.esm`, `Update.esm`, the DLC
masters) are reported as Tier 1 and excluded from the maximum, because their
thousands of `VMAD` records are the baseline the port must consume, not a mod
signal. SKSE/ENB/HDT detection uses specific tokens (`skse`, `enbseries`,
`hdt-smp`, `fnis`, ...), not generic three-letter substrings; an earlier
version matched `enb`/`hdt`/`smp` inside `Skyrim.esm` and falsely reported
Tier 3.

---

## 4. Performance / simplification mods (the focus)

The useful category is **"removes work"**, not "adds a shader". The port should
prefer mods that reduce draw calls, overdraw, particles, AI ticks and texture
residency.

### Types that genuinely help

| Type | Why it helps the 3DS | Examples (category identification only) | Port action |
|---|---|---|---|
| Object/reference removal | fewer draws, fewer NIFs/textures resident | Insignificant Object Remover, "remove useless objects", clutter cleaners | Tier 1 if plugin-only; verify masters; scan reports refs/meshes only at cell level later |
| Occlusion / room bounds | culls before vertex work | Skyrim Project Optimization, eFPS-style occlusion plugins | Tier 1 for the plugin; needs runtime occlusion/frustum integration |
| Grass/landscape reduction | grass is vertex/overdraw heavy and never batched well | No Grassias, "less grass", INI `iMaxGrassTypesPerTexure`/`iMinGrassSize` | INI profile now; plugin edits Tier 1 |
| NPC/creature spawn and greeting reduction | fewer actors = fewer scripts, AI, meshes, voices | No NPC Greetings, less aggressive animals, "no dragons" variants | Tier 1 if plugin-only; some variants add scripts → Tier 2 |
| Particle/fog reduction | overdraw is the PICA bottleneck | Skyrim Performance PLUS (particles), Remove Interior Fog, "no embers" | Texture-only variants need offline downscale/ETC1; plugin variants Tier 1 |
| Low-res / optimized texture packs | fits the 4 MiB texture budget | Optimized Vanilla Textures, low-res packs, "performance textures" | Tier 2: `mod_optimize.py textures --max-dim 128/256 --format etc1` |
| LOD/distant-detail reduction | fewer meshes and smaller working set | "distant detail" removers, low LOD INI | INI profile + mesh-plan; some are plugin edits |

### "Performance" mods that are traps on this port

* **SkyUI / MCM menus** — SkyUI itself needs SKSE for the MCM; custom SWF alone
  is Tier 2, but most "performance" presets assume SKSE. Tier 3 in practice.
* **Anything shipping a `.dll`** — ENB, ReShade, SKSE plugins, Crash Fixes,
  SSE Engine Fixes, `d3d9.dll` wrappers. Tier 3, no loader.
* **"Performance" mods with Papyrus polling** — e.g. mods that clean corpses,
  disable objects or manage weather through a quest script. The scanner counts
  `VMAD`/`SCPT`; ≥100 VMAD is Tier 2. A plugin that removes references is
  cheaper than a script that removes them at runtime.
* **HDT/SMP cloth/hair, Nemesis/FNIS outputs** — Tier 3; they also add CPU and
  memory cost, the opposite of the goal.
* **4K "optimized" packs** — still 4K. Downscale offline or skip.
* **Merged/bashed patches built for SE** — ESL/SE form versions are Tier 3.

### How the port will support them

1. **`performance` profile** (planned entry point): resolve `plugins.txt` with
   only the light mods, keep official masters, mount archives in order, and
   expose a manifest the loader can log.
2. **Default INI policy** for the profile: lower shadow distance/resolution,
   grass density, particle counts, actor fade, LOD distances, and disable
   expensive post effects. These are engine settings, not new shims.
3. **Offline optimization** (`tools/mod_optimize.py`): textures to ETC1/BC1 at
   128/256, mips down to 8×8, optional BSA v104 repack. No source file is
   modified.
4. **Scanner** (`tools/mod_scan.py`): Tier, SKSE/ESL/SE detection, texture
   formats/sizes, VRAM estimate, missing masters, script counts. Run it before
   installing a mod, and again after repacking.
5. **Streaming/eviction** (existing renderer budget hooks): the texture cache
   already trims to the 4 MiB budget; cell assets should be loaded on demand,
   not at startup.

### Recommended first mods (low risk → higher)

1. **Insignificant Object Remover** (plugin only, no SKSE): pure reference
   removal; good first validation of `plugins.txt` + master handling.
2. **Skyrim Project Optimization** (occlusion planes): plugin-only, but check
   its masters — the local install lacks `Dawnguard.esm`/`Dragonborn.esm` files
   even though Steam's `plugins.txt` names them, so a DLC-mastered version will
   (correctly) raise `MissingMasterError`.
3. **No NPC Greetings** (plugin edit): removes dialogue/AI work, tiny plugin.
4. **Grass/INI reduction** (No Grassias or manual INI): no new assets; pairs
   with the future `performance` profile.
5. **Skyrim Performance PLUS** particle textures: texture-only; run
   `mod_optimize.py textures --max-dim 128 --format etc1` and repack.
6. **Optimized Vanilla Textures / a low-res pack**: Tier 2; downscale to
   128–256 and ETC1, then measure. Expect most of the 4 MiB budget to go to a
   single cell's unique textures.
7. **Alternate Start**: *not* a performance mod; listed because it is commonly
   suggested. It is Tier 2 (scripts + quest), useful only after Papyrus exists.

Avoid for now: SkyUI-dependent presets, ENB presets, HDT/SMP, Nemesis/FNIS,
4K packs, SE/ESL merges.

---

## 5. Memory / VRAM budget

Numbers from `docs/MEMORY_BUDGET.md`, `tools/skyrim/budget.py` and
`source/d3d9_texcache.h`:

| Resource | Budget | Consequence for mods |
|---|---:|---|
| Total guest/FCRAM target | 178 MiB | startup threshold measured at 42.1 MiB touched; plugins + indexes + code share it |
| Texture budget (VRAM) | 4 MiB | only ETC1 (4 bpp) / ETC1A4 (8 bpp); BC is not sampleable |
| Total VRAM | 6 MiB | framebuffers + reserve leave 4 MiB |
| Geometry heap | 8–20 MiB | 24 B/vertex, 16-bit indices (6 B/triangle) |
| Texture cache entries | 256 | LRU/eviction with budget trims |
| C reader index caps | 24 MiB index, 64 MiB stored, 96 MiB unpacked | too generous for device; cap per-mod at ~8 MiB index / 8 MiB decode |

Concrete ETC1 sizes (including mips down to 8×8):

| Texture | ETC1 | ETC1A4 |
|---|---:|---:|
| 128×128 | 10.7 KiB | 21.3 KiB |
| 256×256 | 42.7 KiB | 85.3 KiB |
| 512×512 | 170.7 KiB | 341.3 KiB |
| 1024×1024 | 682.7 KiB | 1.33 MiB |

So the 4 MiB budget holds roughly **95 unique 256² ETC1 textures**, or
**380 unique 128²**, before mips of shared atlases. A vanilla exterior cell
with hundreds of unique 1K textures does not fit; streaming is mandatory.
Practical guidance:

* plugins: keep the active set small (tens, not hundreds). LE's hard limit is
  255 plugins; the port's honest comfortable range until the VM/Papyrus exists
  is **≤ 50 active plugins**, and each plugin's bytes count against the guest
  budget (the loader reads headers, not whole plugins).
* BSA index: one 1.4 GB `Skyrim - Textures.bsa` has a large directory; per-mod
  archives should be pruned or merged. The C reader charges 24 MiB worst case;
  the scanner warns above 8 MiB.
* textures: downscale to 128 for particles/clutter, 256 for architecture,
  keep 512 only for UI/hero assets. Normal maps double the cost (ETC1A4) if
  alpha is preserved; consider dropping the alpha channel for mods that do not
  need it.
* meshes: 100k triangles ≈ 2.4 MB vertex + 0.6 MB index, plus the NIF staging
  copy. Mods adding high-poly clutter are Tier 2 and usually not worth it on a
  performance profile.

`mod_scan.py` computes `vram_etc1_bytes_at_cap` at 256 px by default and emits
`vram-over`, `plugin-bytes`, `bsa-index`, `texture-count` warnings. The numbers
are estimates from DDS headers and entry counts, not a runtime measurement.

---

## 6. Tools and tests

```powershell
# inspect the current Data/load order and resolve an asset
python tools/mod_scan.py --data F:\Skyrim\Data --json scan.json

# scan an unpacked mod (Data/ or mod root), deep mesh parse optional
python tools/mod_scan.py --mod-dir .\mods\LightMod --deep-mesh --json mod.json

# downscale/convert a texture tree to ETC1 (source untouched)
python tools/mod_optimize.py textures .\mods\LightMod\textures .\build\etc1 `
  --max-dim 128 --format etc1

# single texture, BC1 instead of ETC1
python tools/mod_optimize.py texture .\in.dds .\out --max-dim 256 --format bc1

# repack as a v104 BSA (port reader; no TES4 hashes)
python tools/mod_optimize.py pack .\build\LightMod.bsa .\build\etc1\*.sktx

# report mesh LOD/decimation candidates (never rewrites)
python tools/mod_optimize.py mesh-plan .\mods\LightMod\meshes
```

Tests (`tests/mod_compat_test.py`, 23 cases, synthetic only):

* synthetic ESP with masters/flags/form version; Tier 1/2/3 classification;
* plugins.txt parsing (BOM/comments/stars/duplicates/paths) and toposort;
* missing master and missing plugin named errors; master auto-activation;
* loose > BSA, later plugin > earlier plugin, `Update.bsa` last;
* BSA writer/reader roundtrip, v103 header acceptance, bad version;
* DDS scan buckets and VRAM estimate; ETC1 downscale with decoded PSNR > 20;
  BC1 DDS output; pack roundtrip; optimizer leaves the source bytes intact.

Run:

```powershell
python -m unittest tests.mod_compat_test -v
python -m py_compile tools/skyrim/data_overlay.py tools/skyrim/bsa_write.py `
  tools/mod_scan.py tools/mod_optimize.py
C:\devkitPro\devkitARM\bin\arm-none-eabi-gcc.exe -std=c11 -D_GNU_SOURCE `
  -Wall -Wextra -Werror -Isource -c source/bsa_reader.c -o bsa_arm.o
```

---

## 7. Known blockers / next step

* **Papyrus VM, records/cells, collision, AI, animation graph**: not present.
  Tier 2 script mods and Alternate Start remain blocked; no date promised.
* **Runtime overlay wiring**: `DataOverlay` is Python; the C runtime currently
  mounts only `Skyrim - Interface.bsa` + `Update.bsa` through
  `menu_bsa_runtime.c`. The next code step is a C mount list emitted from the
  manifest (or a small C loader that reads plugins.txt) so the same precedence
  is used on device.
* **Texture repack path**: ETC1/BC1 outputs exist, but the asset staging route
  into the 3DS build is not automated yet.
* **Mesh decimation**: analysis only. Implementing a vertex/index rewrite needs
  a validated NIF writer and a visual/collision check; do not fake it.
* **Per-mod index cap**: lower the C defaults (24/64/96 MiB) to the device
  numbers before loading real mod archives.
