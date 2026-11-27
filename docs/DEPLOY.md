# Device deployment: `tools/deploy_device.py`

Status: **file layer only.** These commands copy and convert assets; they do not
boot the game and claim no frame. No game file is stored in the repository —
both commands read the install the user points them at.

The tool answers two questions:

1. *What would a device copy contain, and what does it cost?* (`plan`, read-only)
2. *Write that copy and convert what the port cannot load.* (`stage`)

## 1. Usage

```powershell
# read-only report + JSON
python tools\deploy_device.py plan --data F:\Skyrim\Data `
    --tesv F:\Skyrim\TESV.exe --ir E:\census_ir2\ir_container\tesv.irc `
    --json E:\deploy_plan.json

# write the device tree
python tools\deploy_device.py stage --data F:\Skyrim\Data `
    --tesv F:\Skyrim\TESV.exe --ir E:\census_ir2\ir_container\tesv.irc `
    --out E:\stage\Skyrim
```

Layout produced by `stage`:

```
<out>\TESV.exe
<out>\deploy_manifest.json
<out>\Data\<active plugins>
<out>\Data\<mounted *>.bsa
<out>\Data\Video\*.bkr
<out>\Data\<loose mod files>
<out>\Data\<converted textures>
<out>\ir\tesv.irc
```

Copy `<out>` to the card as `sdmc:/Skyrim` (FAT32). `deploy_manifest.json` lists
every copied file with size and SHA-256.

Options:

| flag | meaning |
|---|---|
| `--resize none` (default) | never touch resolution |
| `--resize half` | cap the largest dimension at 512 |
| `--resize quarter` | cap at 256 |
| `--skip-archives` | do not copy the BSA files (report only) |
| `--plugins`, `--loadorder` | explicit plugin lists instead of auto-discovery |
| `--full-rows` | include per-texture rows in the plan JSON |
| `--compress` | zlib-compress every cooked payload (`--pack cooked` only) |
| `--compress-level N` | deflate level 0..9 (default 9; inflate cost is flat) |
| `--compress-sample-mb N` | `plan --compress`: raw bytes sampled per group (default 8, 0 disables) |

## 2. What wins

Winners come from the same resolver the runtime uses
(`tools/skyrim/data_overlay.py`):

* load order is built from `plugins.txt` / `loadorder.txt` / `MAST` sort;
* a later plugin's archive wins over an earlier one;
* loose files win over every archive;
* a path that loses is counted under `resolve.overridden_entries` — it is the
  size a future cooked pack could drop.

### Runtime mount list

The C runtime reads the archive names from `Skyrim.ini` **at boot**.  The menu
loader (`source/menu_frame_main.c`) calls `menu_bsa_sources_from_boot(
"sdmc:/Skyrim")`, which tries `sdmc:/Skyrim/Skyrim.ini` first and then
`sdmc:/Skyrim/Data/Skyrim.ini` (the cooked tree's rewritten INI), reads
`[Archive]`, and mounts `sResourceArchiveList` first, then
`sResourceArchiveList2`, in file order.  A cooked tree that names only its
cooked archives therefore mounts exactly those.

The list is capped at 32 archives and a 96 MiB summed index; an over-limit list
is a named refusal.  A missing or corrupt archive is logged and skipped, and an
absent/empty list falls back to `Skyrim - Interface.bsa` then `Update.bsa`.
Startup writes `[menu-bsa] N archive(s) from Skyrim.ini: ...` or
`[menu-bsa] fallback: Interface+Update` to the device stderr log.  See
`docs/MENU_BSA_RUNTIME.md`.

## 3. Texture policy

| source | action |
|---|---|
| DXT1 / DXT3 / DXT5 (BC1/2/3) | **keep**, byte-for-byte, no re-encode |
| X8R8G8B8 / A8R8G8B8 (bgra8/bgrx8) | **keep** |
| 24-bit BGR/RGB | **convert** to DXT1 (alpha-aware RGBA8 fallback) |
| non-power-of-two | **convert** to RGBA8 at the nearest lower POT |
| BC4 / BC5 | **refuse** — no encode path on the device |
| R8 / volume / other | **refuse**, named |

Every refusal is listed with its reason in the plan. Refused textures are not
copied by default, so the runtime reports a missing texture instead of loading
a format it cannot sample.

`--resize` is an emergency lever, not the default: performance-texture mods
already ship small files, and the 400x240 screen does not reward 1024². The
measured policy keeps 18,524 of 18,714 textures untouched.

## 4. Measured on the reference install

`plan --data F:\Skyrim\Data` (11 s):

| | |
|---|---:|
| unique assets | 71,266 |
| textures | 18,714 |
| DXT1 / DXT3 / DXT5 | 15,379 / 627 / 2,517 |
| BGR24 / BGRA8 / R8 | 189 / 1 / 1 |
| keep / convert / refuse | 18,524 / 189 / 1 |
| texture bytes (winning sources) | 1.371 GB |
| converted estimate | 6.2 MB |
| core + archives copied | 4.50 GB in 14 files |
| entries hidden by higher priority | 124 archive + 3 archive-over-loose |

The single refusal is `textures\effects\noisevolume.dds` (R8 volume noise).

## 5. Cooked archives (`--pack cooked`)

`--pack cooked` replaces the whole-BSA copy with new archives that contain
**only the winning assets**, already converted:

* winners are grouped by their top-level directory (`textures`, `meshes`,
  `sound`, `scripts`, …);
* a group larger than `--max-archive-mb` (default 1800) is split into
  `Skyrim - Cooked <group> 2.bsa`, …;
* plugins, `*.ini`, `*.txt`, `*.bik` and `*.bkr` stay outside the archive;
* archives are written **uncompressed by default** — on ARM11 the CPU is more
  expensive than SD read time; `--compress` opts into zlib (see §5.1);
* `Data\Skyrim.ini` is rewritten with
  `sResourceArchiveList=<cooked archives>` and an empty
  `sResourceArchiveList2`;
* the original vanilla BSAs are not copied at all.

Measured with `plan --pack cooked` on the reference install: **70,001 files /
4.16 GB / 3 archives / 1 refusal**. Groups: meshes 1.57 GB, textures 1.39 GB,
sound 0.86 GB, grass 0.16 GB, music 0.10 GB, everything else < 50 MB.

The writer is `skyrim.bsa_write.write_bsa_stream`: payloads are staged in a
temporary file and only metadata stays in RAM, so a multi-gigabyte archive does
not need a matching Python heap. It emits the same v104 layout as `write_bsa`.

### 5.1 Compression (`--compress`)

`--compress` makes every cooked payload zlib-compressed; `--compress-level`
(0..9, default 9) only changes the cook, not the runtime inflate cost. The
on-disk format is exactly the retail one: archive flag `0x004`, then per file
`u32 unpacked_size` + the deflate stream (after the optional embedded-name
prefix). `BsaArchive.read_file()`, the C `bsa_read()`/`bsa_read_prefix()` and
`stream_index.verify_index()` inflate and hash the unpacked bytes, so a
compressed stage is validated like an uncompressed one.

`plan --compress` projects the saving without staging 6 GB: it samples the
winning sources per group, byte-weighted so a 5 MB texture and a 40 KB tint map
do not count equally, and caps the work at `--compress-sample-mb` (default 8)
per group. Measured on the current `E:\SteamLibrary\...\Skyrim\Data` install
(with mods; 113,285 cooked files, 6.64 GB raw / 13 archives, 68.9 MB sampled):

| group | raw MB | projected MB | saved |
|---|---:|---:|---:|
| meshes | 2542.5 | 1194.7 | 53% |
| textures | 1955.1 | 1159.1 | 41% |
| sound | 1744.6 | 1401.4 | 20% |
| grass | 186.8 | 151.1 | 19% |
| music | 123.0 | 121.6 | 1% |
| strings | 40.3 | 15.4 | 62% |
| shadersfx | 27.5 | 1.8 | 94% |
| scripts | 16.7 | 6.0 | 64% |
| **total** | **6639** | **4053** | **39%** |

The sample ratio is 0.626, projected saving ≈ 2.59 GB. This is a plan
estimate: converted textures are sampled as their source bytes, and the
8 MB/group cap keeps the projection noisy for the smallest groups. A real
`stage --compress` is the authority.

When it pays:

* **Yes — meshes, shadersfx, scripts, strings, seq.** Structured/text payloads
  save 40–94%, and the CPU cost is paid on load, not per frame.
* **Depends — textures, sound, grass.** ~20–40% saved, but every DDS/WAV load
  pays a full ARM11 inflate, and BC DDS / PCM are already entropy-coded.
  Enable only when card space is the binding constraint.
* **No — music, interface.** 1–3% saved for the same CPU; leave uncompressed.
* **Never for streamed groups.** `bsa_read_range()` returns
  `BSA_ERR_UNSUPPORTED` for a compressed entry, so a resource read through the
  range path must stay uncompressed. `stream_index.bin` is still written and
  every compressed row carries flag bit 0 with its stored deflate range; the
  device can see the row but must fall back to `bsa_read()` or a loose file.
  Prefer compressing groups the loader reads whole; keep streamed geometry
  uncompressed.

The C reader already refuses compressed ranges (it reads the archive header
flag, not the index), so a mixed pack is safe: compressed groups fail the range
path by name instead of returning deflate bytes.

Trade-offs:

* changing the load order or a mod requires a **re-cook** on the PC;
* a per-file patch can still be dropped as a loose overlay — the runtime
  overlay lets loose files win;
* the C mount list reads the archive names from the INI at boot (game-root
  `Skyrim.ini`, then the cooked `Data/Skyrim.ini`); no separate manifest is
  needed.

## 6. Limits / next steps

* **BSAs are copied whole.** Overridden vanilla entries still occupy space
  (only 127 in the reference install). A cooked single-archive mode that writes
  only winners is the next step; `mod_optimize.pack_bsa` already provides the
  writer.
* **No mesh conversion** — NIF/LOD optimisation is `mod_optimize.py mesh-plan`.
* **No R8/volume textures.** If a real effect needs one, it needs a loader path
  first, not a silent conversion to RGBA8.
* **Loose mod files are copied as-is** except textures; script/mesh format
  checks stay with `mod_scan.py`.
