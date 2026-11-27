# CELL_TRACE - where the engine fixes the current cell, and where to hook prefetch

Status: **static disassembly only, no run claimed.** Worktree `cell-trace`,
base `cbe2f9b`. Scope: `docs/` + `tools/`, no C changes.
Addendum 5.4 (schema 2 exterior rows) belongs to the `world-index` branch and
is implemented in `source/asset_stream.{h,c}` + `tools/stream_index_build.py`.

Image: `E:\census_ir2\ir_container\_work\ir_container_input.bin`
(SHA-256 `13B8D2779F1F93FFDB9A83C0DFC71CDDD1CB484E8994223EF957A87F616A052E`);
`off = 16 + VA - 0x401000`; header `<IIII` = magic `0x49524D31`,
lo `0x401000`, code size `0xc6931a`, entry `0x69d1d0`; function bounds from
`E:\Skyrim_N3DS_build\analysis\tesv\functions.json`. Strings/`.data` are not in
the container, so INI keys come from the on-disk PE
(`E:\SteamLibrary\steamapps\common\Skyrim\TESV.exe`, same VAs).

Tools (same shape as `world_*`):

| tool | purpose |
|---|---|
| `tools/cell_dis.py` | disassemble a function/range; `--off 0x6c` shows only accesses to that struct offset, `--calls` only call/jmp |
| `tools/cell_xref.py` | one-pass xref index (`--build` -> `%TEMP%/cell/xref.json`), query `calls` / `imms` / `mems` / `writes` |

Example:

```
python tools/cell_dis.py 0x4397e0                       # whole owning function
python tools/cell_dis.py 0x4397e0 --off 0x6c            # struct field accesses
python tools/cell_xref.py --mems 0x12e3560 0x1ba7738
python tools/cell_xref.py --writes 0x1b2e864
```

---

## 1. TL;DR: the three addresses worth using

| what | address / field | evidence |
|---|---|---|
| **world/player-side holder** (re-read the pointer every frame; it is written once) | `[0x1b2e864]` | only static write: `0x6991a0` inside `0x699100`; read by ~250 functions; used as the "fallback cell" source in `0x693860` |
| **current cell** | `[[0x1b2e864] + 0x6c]` | `0x43a265` `mov [esi+0x6c], ebp` (new cell), `0x43a13a` compares old/new, `0x4397f8/0x439827` use it, `0x69389b` `esi = [ecx+0x6c]` = "the cell the player is in" |
| **current grid coords** (exterior) | `+0x5c` = X, `+0x60` = Y (int, cell units, `pos >> 12`) | `0x43a360..0x43a378` writes them (`sar 0xc`), `0x43398b/0x4339a1` use them as the uGrids centre, `0x439c06..0x439c1d` update them and keep the delta |
| previous grid coords | `+0x64` = X, `+0x68` = Y | `0x439b55/0x439b58`, `0x439c12/0x439c15` |
| current worldspace (NULL = interior) | `+0xbc` | `0x43a33a`, `0x433980` (`ecx=[esi+0xbc]` -> `0x4f5330`), `0x439d3e` |
| cell grid container / map | `+0x40` | `0x438e...`, `0x69c...`, `0x6938a7` uses `[obj+0x40]` to look up the centre cell |
| **"grid/cell geometry changed" flag** | `0x12e3560` (byte) | set to 1 at `0x439cbf`, cleared at `0x4397fe` and `0x43a84b`, read at `0x4397e3` and `0x434590` |
| "cells updated" flag | `0x1ba7738` (byte) | set at `0x439cc6`, cleared at `0x43a852` / `0xca0e16`, read at `0x4397ef` |
| "load in progress" | `0x12e3561` (byte) | see `docs/LOADING_MAP.md` |
| `uGridsToLoad` runtime value | `0x1241000` | `0x433983` (ring size), `0x6938a2` (`uGrids/2` centre lookup), `0x434153`, `0x434e45` |
| worldspace "current cell" cache | `[worldspace + 0x94]` | `0x4f06e0` lazily creates it |
| cell type flags | `[cell + 0x2c] & 1` = interior | `0x693930`, `0x4f54ea`, `0x6984a7` |
| cell worldspace back-pointer | `[cell + 0x84]` | `0x4f54f4`, `0x693959` |

Consequence: a host-side prefetch trigger does **not** need a guest call hook.
Once per frame, after the guest frame function returns, read
`h = guest_read32(0x1b2e864)`, then `cell = read32(h+0x6c)`,
`gx = read32(h+0x5c)`, `gy = read32(h+0x60)`, `ws = read32(h+0xbc)`.
A change of `(cell, gx, gy, ws)` is a real cell transition. `0x12e3560` is the
engine's own latch for "the grid moved this update".

---

## 2. Where the cell is committed

### 2.1 Normal walking: per-frame grid update `0x4397e0`

Called from the frame function at **`0x69cfe3`** (`fn 0x69cbd0`), with `ecx =
[0x1b2e864]`. Other callers: `0x43a14a` (`0x43a120` same-cell fast path),
`0x43a68d` (`0x43a530`), `0x694a4c`, `0x67d5b6`.

```
0x4397e0  cmp [0x12e3560],0 ; je 0x43980a        ; consume the "grid moved" latch
0x4397ef  cmp [0x1ba7738],0 ; jne 0x43980a       ; unless "cells updated" is pending
0x4397f8  cmp [esi+0x6c],0  ; jne 0x43980a       ; unless a cell is already attached
0x4397fe  [0x12e3560] = 0
...
0x439bd8  ; position argument -> grid
0x439bee  sar edi,0xc ; sar ebp,0xc              ; world pos -> cell coords
0x439c00  (inside-cell teleport / no-grid mode keeps old coords)
0x439c06  eax = edi - [esi+0x5c]                 ; delta X
0x439c0b  [esi+0x5c] = edi                       ; ***new grid X***
0x439c12  [esi+0x64] = edi
0x439c15  [esi+0x68] = ebp
0x439c1a  eax = ebp - [esi+0x60]                 ; delta Y
0x439c1d  [esi+0x60] = ebp                       ; ***new grid Y***
0x439c2a..0x439c78  update 0x1ba7724..0x1ba7730  ; grid centre in world units
0x439cbf  [0x12e3560] = 1                        ; ***"grid moved" event***
0x439cc6  [0x1ba7738] = 1
0x439ccd  0x434550(esi)                          ; consumes 0x12e3560 (LOD/terrain refresh)
0x439cd2  0x4b49d0/0x4b4b30/0x4b4ca0([0x12e7654]) ; cell-manager housekeeping
0x439cf7  if ([esi+0x6c] != 0 && !interior-change) -> 0x439fe7
0x439d3e  if (![esi+0xbc]) { cell = 0x43bcb0(...); 0x433a70(esi, cell) }  ; interior
0x439e07  0x438340(esi, ws, gx, gy)               ; "is cell already present"
0x439e33  if (|dX| > 1 || |dY| > 1) 0x433980(esi, 0)  ; full uGrids ring rebuild
0x439ea4.. walk dX x dY rectangle, 0x432d80(cellX, cellY) + 0x12e35cc ...
0x439e4e  0x436410(esi)
```

`0x433980` is the ring loader: it reads `[esi+0xbc]` (worldspace), subtracts
`uGrids/2` from `[esi+0x5c]/[esi+0x60]`, and for every grid coordinate in the
`uGrids x uGrids` square:

```
0x4339e0  switch(arg): 0 -> 0x474470(gridmap,x,y) (already present?)
                        1 -> 0x4743f0(gridmap,x,y)
0x433a0a  ecx = [esi+0xbc]                       ; worldspace
0x433a26  cell = 0x4f5330(worldspace, x, y)      ; ***create/load exterior cell***
0x433a2f  if (!cell) 0x43ef60(0x12e2cb8, 1, x, y, "uGridsToLoad"?)  ; fallback
```

`0x4f5330` (`TESWorldSpace::CreateCellAtGrid`) is the actual cell constructor
path:

```
0x4f5330  key = (x<<16)|y ; 0x4375d0(worldspace, key)  ; existing cell?
0x4f5379  [0x12e563c] = worldspace                     ; "last grid worldspace"
0x4f53e0  for i in [worldspace+4]..                      ; cell iterator
0x4f53ec  0x4515b0 / 0x4472d0                            ; cell -> refr list
0x4f5411  0x4f2dc0(worldspace, cell, x, y)               ; attach to grid
0x4f542a  alloc 0x90 -> 0x4cc2b0/0x4cd530                ; TESObjectCELL ctor
0x4f5444..0x4f5459 0x4c0240/0x4c1820/0x4c0550            ; init cell, set grid
0x4f5461  0x4f3f10(worldspace, cell)
0x4f5476  0x43b4a0(cell, refr)                           ; add refr to cell
0x4f5481  0x4cca50(cell, refr)
0x4f54c5  0x4f44a0(worldspace, cell)                     ; register in grid
0x4f54e1  0x4f06e0(worldspace) -> current cell; if equal -> 0x4ad090(ws+0x170, cell)
0x4f550a  0x4cd5e0(cell)
```

So an exterior cell enters the world through `0x4f5330` -> `0x4f2dc0`/
`0x4f44a0`, and its references through `0x43b4a0`/`0x4cca50`.

### 2.2 Cell switch with a position (teleport, load, starting cell) `0x43a120`

```
0x43a120 (ecx = [0x1b2e864], args: cell, pos*)
0x43a13a  if ([esi+0x6c] == cell) -> 0x4397e0(cell, pos)   ; same cell, just move
0x43a263  [esi+0x6c] = cell                                ; ***commit new cell***
0x43a290..0x43a2fc  clear/rebuild grid map state
0x43a33a  if ([esi+0xbc]) 0x4f06e0(ws)                     ; worldspace current cell
0x43a360  fld pos[0]; sar 0xc; [esi+0x5c] = gx             ; grid from position
0x43a36d  fld pos[1]; sar 0xc; [esi+0x60] = gy
0x43a3b4  if (cell data pending) 0x4352d0(1,1,cell)        ; loading screen!
0x43a4db  if ([esi+0xad] || [esi+0xae]) 0x4352d0(1,0,0,0)  ; loading screen!
0x43a4bb  0x4330e0(esi, cell)
```

Callers: `0x693860` (startup location, at `0x69ddde` in WinMain),
`0x6982c0` (starting cell from INI), `0x742b61` (three call sites - script/console
"move to cell" style path).

`0x43a530` is the interior/single-cell variant: it stores `[esi+0x6c]=0`
(`0x43a5ab`), clears both latches (`0x43a84b`, `0x43a852`), calls `0x432d80`,
`0x4352d0` (`0x43a6fa`, `0x43a7e0`), `0x434550`, `0x4397e0`.

### 2.3 Startup location notification `0x693860`

```
0x693860(cell):
  if (!cell) cell = [[0x1b2e864]+0x6c]
  if (!cell) cell = [[0x1b2e864]+0x40][uGrids/2, uGrids/2]
  cmp cell, [0x12e27c4]        ; "last announced cell" - read-only, never written
  0x4a4b70(&loc)               ; build location struct
  interior ? worldspace=0 : [cell+0x84]
  0x734b00/0x746dd0/0x4e3370([0x1b2e8e4], &loc)   ; location-changed notification
```

`0x12e27c4` has **no static writer** (checked with `cell_xref.py --writes
0x12e27c4`), so it is dead as a latch in this build; do not build a trigger on
it. The event that matters for streaming is `0x12e3560` / the `+0x5c/+0x60/+0x6c`
tuple.

---

## 3. Relation to `0x4352d0` (loading) and `0x69cbd0` (frame pump)

`0x4352d0` is the load **state machine**, not a file loader (full map in
`docs/LOADING_MAP.md`):

```
0x4352d0(1,...) -> 0x435345: [0x1b2d4c0]=1, menu hide, 0x6f3f80(1)
                            -> 0x6f3ea0(2) -> mode 2 -> ReleaseSemaphore(0x537)
                            0x12e3561 = 1   ("loading")
0x4352d0(0,...) -> 0x435455: [0x1b2d4c0]=0, 0x6f3f80(0) -> 0x6f3f00(2)
                            0x12e3561 = 0   ("loading finished")
```

While `[0x1b39124] == 2`, the stream thread runs `0x6f4000` (menus + present)
instead of the game frame. `0x69cbd0` (called from WinMain `0x69dfe7`) is the
frame that owns cell streaming:

```
0x69cbd0
  0x69cc12 0x6986c0
  0x69cc17 0x460580
  0x69cc49 0xa5b620 -> 0xa5b3c0           menu tick
  0x69cc6e [0x1b2e85f] = has player/world
  0x69cc8e 0xa5c7d0 -> [0x1ba75f8]
  0x69cdfc paused? -> 0x69ce1d
  0x69ce15 0x6f3ea0(1)                    mode 1, one token/stream
  0x69cfb0.. 0x69cfe3 0x4397e0([0x1b2e864]) ***cell grid update / streaming***
  0x69d0aa 0x69c930([0x1b2e864])          world/menu stage
  0x69d0c3 0x6f3f00(1)                    flush, waits 0x538/stream
```

Verified trigger points and their meaning:

| site | meaning |
|---|---|
| `0x69cfe3` `call 0x4397e0` | every gameplay frame; grid coords are recomputed and cells are (un)loaded here |
| `0x439cbf` `[0x12e3560]=1` | the grid centre moved by >=1 cell this update (the "entered a cell" edge) |
| `0x439e37` `call 0x433980` | full `uGrids` ring rebuild, only when the delta in either axis is > 1 |
| `0x43a265` `[obj+0x6c]=cell` | a cell is committed by id (teleport/interior/startup) |
| `0x4353ba` `0x6f3f80(1)` | loading screen takes the frame (mode 2); does *not* read files itself |
| `0x43548b` `0x6f3f80(0)` | loading screen released (mode 0) |

Important: crossing a cell boundary while walking normally does **not** go
through `0x4352d0`; it is absorbed by `0x4397e0`/`0x433980` and the async
reference loader. `0x4352d0(1)` appears only for interior/first load and for
explicit loads (`0x43a3b4`, `0x43a4db`, `0x43a6fa`, `0x43a7e0`, `0x746e61`,
`0x699530`, `0x69de22`, ...). Do not gate prefetch on the loading mode: it is 0
during normal walking.

---

## 4. What is read on a cell transition

There is no static list of file names - the engine resolves them from records at
run time. Statically the chain is: cell -> `TESObjectREFR` list -> each ref's
`TESModel`/`TESTexture` -> renderer load -> BSA entry -> `ReadFile`. In this port
that last step is the shim's `s_ReadFile` (`source/win32_shims.c:1838`), which is
the only place where guest I/O becomes host I/O.

Categories that a cell transition pulls (all through the same `ReadFile` path):

| kind | record -> path | notes |
|---|---|---|
| cell record itself | `Skyrim.esm` (CELL/REFR/NAVM) | already opened by the ESM shim; no per-cell file |
| meshes | `TESModel` (`MODL`) -> `meshes\...nif` | one NIF per placed ref + cell "landscape" NIF |
| landscape/terrain | `LTEX`/`TXST`, `meshes\landscape\...` | exterior only |
| textures | `TESTexture` (`ICON`/`TX01`) + material `TXST` -> `textures\...dds` | bound by the NIF loader |
| LOD | `meshes\terrain\<world>\...btr` (near), `...bto` (far), `_lod.nif` | exterior only, step 0x434550/0x1ba7724 region |
| worldspace grid | `0x4f5330` creates empty cells for grids with no data | no file read for empty cells |

The cooked pipeline reserves a per-cell table: `STREAM_INDEX_CELL_ENTRY_SIZE`
= 64 bytes (`key[24] | resource_count u32 | flags u32 | 8 x u32 resource id`).
Schema 1 left `cell_count = 0`; schema 2 (`tools/stream_index_build.py`) fills
it for interiors (`kind=0`, `form_id`) and exterior windows (`kind=1`,
`(worldspace_form_id, grid_x, grid_y)`), so cell prefetch is a data problem
that is now solved for both branches (`source/asset_stream.{h,c}`,
`docs/STREAMING.md`).

---

## 5. Integration plan for `asset_stream` (P0 prefetch)

### 5.1 Trigger (host layer, no C changes needed to prove it)

Poll once per frame, *after* the guest returns from `0x69cbd0` (or after
`0x4397e0` if a guest hook exists):

```c
uint32_t holder = guest_rd32(0x1b2e864);      /* re-read, never cache */
if (holder) {
    uint32_t cell = guest_rd32(holder + 0x6c);
    int32_t  gx   = (int32_t)guest_rd32(holder + 0x5c);
    int32_t  gy   = (int32_t)guest_rd32(holder + 0x60);
    uint32_t ws   = guest_rd32(holder + 0xbc); /* 0 = interior */
    uint8_t  moved = guest_rd8(0x12e3560);     /* engine's own edge flag */
}
```

Fire when `(cell, gx, gy, ws)` differs from the previous sample, **or** when
`moved == 1`. Both are cheap; the tuple is the accurate one (survives a
`0x4352d0` load that resets `0x12e3560` in `0x43a84b`).

Order of work per transition:

1. `uGrids = guest_rd32(0x1241000)` (clamp 1..7 like `0x433980`).
2. Build the `uGrids x uGrids` neighbourhood around `(gx, gy)` (or the single
   interior cell `cell`).
3. Map each grid key to the cell row in `stream_index.bin`
   (`cell key -> resource ids`); skip empty rows (no data -> the engine creates
   an empty cell, no I/O).
4. `stream_mount_prefetch_ex(mount, res, n, lod, STREAM_PRIO_P0, deadline, ...)`
   for the **centre cell first**, then the 4-neighbourhood, then the rest of the
   ring. Keep the P0 window bounded (e.g. one cell per frame, ~256 KiB per
   resource, `stream_mount_pump` budget unchanged).
5. `stream_mount_mark_visible` for the centre cell, `stream_mount_trim` when the
   ring shrinks (cell unloaded) - the eviction side of the same event.

### 5.2 Guest-hook alternative (if/when C changes are allowed)

* `0x4397e0` entry: full grid update; too coarse for "which cell", but a good
  place to snapshot the pre-update grid.
* Better: hook `0x4f5330` entry (`ecx` = `TESWorldSpace*`, stack `x`, `y` at
  `[esp+8]`/`[esp+0xc]` after `ret 8`) - this is exactly "this grid cell is
  being realised", and it is the same function used by the startup path.
* Cheapest reliable: watch the store at `0x439cbf` (`[0x12e3560]=1`) - one
  instruction, no calling convention questions.

### 5.3 What to measure

| metric | source | why |
|---|---|---|
| transitions/frame | tuple change count | baseline cell churn |
| frame of `0x12e3560` edge vs frame of first `s_ReadFile` for a cell NIF | host trace timestamps | prefetch lead time actually available |
| P0 queued / ready before demand | `StreamMetrics` + `StreamRequestInfo.state` | did prefetch win the race |
| demand hits on already-READY windows | `requested_priority` vs `priority` boost | dedup working |
| `physical_reads`, `bytes`, `misses` per transition | `stream_mount_metrics` | cost of a transition |
| `s_ReadFile` bytes with `n >= 65536` around a transition | existing `[io] read` lines in `win32_shims.c` | which loader reads dominate |
| evictions/trim after ring shrink | `cancelled`, `bytes_freed` | avoid thrash at cell borders |

Success criterion: after a transition, the centre cell's meshes/textures are
already `STREAM_REQ_READY` before the guest asks for them, and no increase in
peak staged bytes beyond one cell window.

### 5.4 Schema 2: filling exterior rows

`tools/stream_index_build.py` reads the WRLD records and remembers which world
owns each CELL while walking the GRUPs:

| WRLD subrecord | meaning |
|---|---|
| `EDID` | worldspace name (`Tamriel`, `WhiterunWorld`, ...) |
| `DATA` | one-byte flags (not bounds; for example Tamriel has 0) |
| `NAM0`, `NAM9` | min/max bounds, two floats each (`x,y`) |

Exterior CELLs are only counted when they sit under GRUP type 4
(`Exterior Cell Block`) or 5 (`Exterior Cell Sub-Block`). The world's
persistent CELL (Tamriel `0x00000d74`) is a direct child of the World Children
group and carries a placeholder `XCLC 0,0`; treating it as grid `(0,0)` would
collide with the real cell `BleakwindBasinExterior01` (`0x000099a2`), so it is
excluded.

Build a bounded window (no whole-world index):

```
python tools/stream_index_build.py `
  --data "E:/SteamLibrary/steamapps/common/Skyrim/Data" `
  --world Tamriel --grid 5,-11 --radius 1 `
  --out build/stream_index_riverwood_world.bin `
  --report build/stream_index_riverwood_world.json
```

The row key is `(kind=1, form_id=0, gx, gy, worldspace_form_id)`; the trigger
reads exactly `gx = [holder+0x5c]`, `gy = [holder+0x60]`,
`world = [[holder+0xbc]+0x0c]`. Resources are the same `REFR -> NAME ->
MODL -> NIF -> DDS` chain as interiors. Not emitted, and recorded in the
report: `.btr`/`.bto`/`_lod.nif` (their names need the terrain LOD grid),
`LAND`/`LTEX` terrain geometry/textures, and leveled lists.

---

## 6. Open questions / caveats

* All of the above is static. The object at `[0x1b2e864]` is written once at
  `0x6991a0`; its size/identity is inferred from field use (`+0x631d` in
  `0x43a133`, `+0x5c/0x60` grid, `+0x6c` cell, `+0xbc` worldspace). Confirm the
  pointer is non-null and stable in the first guest frame before trusting it.
* `0x4f06e0` caches a worldspace "current cell" at `[ws+0x94]`; this can be a
  different (lazily created) cell than `[holder+0x6c]`. Prefer `+0x6c` for "the
  cell the player is in", `0x4f06e0` only for "the worldspace's grid cell".
* Interior transitions set `+0xbc = 0` and commit through `0x43a530` /
  `0x43a120`, so the neighbourhood is exactly one cell - do not build a grid
  there.
* `0x12e27c4` looks like a location latch but has no writer in this image.
* LOD selection (`0x434550`, `0x1ba7724..0x1ba7730`) is updated in the same
  block; do not treat "grid moved" as "cell changed" for LOD purposes.
