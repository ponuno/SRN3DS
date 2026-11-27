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

## 2. What wins

Winners come from the same resolver the runtime uses
(`tools/skyrim/data_overlay.py`):

* load order is built from `plugins.txt` / `loadorder.txt` / `MAST` sort;
* a later plugin's archive wins over an earlier one;
* loose files win over every archive;
* a path that loses is counted under `resolve.overridden_entries` — it is the
  size a future cooked pack could drop.

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

## 5. Limits / next steps

* **BSAs are copied whole.** Overridden vanilla entries still occupy space
  (only 127 in the reference install). A cooked single-archive mode that writes
  only winners is the next step; `mod_optimize.pack_bsa` already provides the
  writer.
* **No mesh conversion** — NIF/LOD optimisation is `mod_optimize.py mesh-plan`.
* **No R8/volume textures.** If a real effect needs one, it needs a loader path
  first, not a silent conversion to RGBA8.
* **Loose mod files are copied as-is** except textures; script/mesh format
  checks stay with `mod_scan.py`.
