# INIT_TRACE - TESV entry -> CRT/C++ initialization -> WinMain -> D3D9

Status: **diagnostic analysis, not a boot claim.** Written 2026-11-24 while the
supervised growth loop was between growth22 (completed) and growth23
(in progress). No growth script, generated slice, runtime dispatch or shared
build list was edited.

Binary: `E:\skyr\TESV.exe`
SHA256 `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`
(1.9.32.0.8, unprotected same-build copy; see `docs/STATUS.md`).

Evidence classes used below:

| Mark | Meaning |
|---|---|
| **M** | Measured: a host run log, a native Windows execution stack, or a PE field. Exact. |
| **S** | Static: decoded from the PE image and expected on this path. Not observed in this run. |
| **I** | Inference: role or naming from code shape. Explicitly not measured. |

The stable measured slice was `analysis/growth21/iter04`
(33,646 functions, SHA256 `d82c3fc5...8ba6`). The newest **completed measured**
slice at the time of writing is `analysis/growth23/iter03`
(33,672 functions), whose fault is `0x00ff3660`; `growth23/iter04` is the next
stage. The measured sequence `0xff2f80 -> 0xff3000 -> 0xff3240 -> 0xff3260 ->
0xff33c0 -> 0xff33f0 -> 0xff3660` matches the table order predicted by this
analysis and is the strongest evidence that the frontier is a linear walk of
`0x106b464..0x1076a98`.

Helper tools (new files, read-only, non-production):

- `tools/init_trace.py` - enumerate/verify CRT tables, bounded call tracing,
  frontier arithmetic, D3D9 call-site discovery, host-run annotation.
- `tools/annotate_host_run.py` - turn a saved `probe_run.log` (+ profile CSV)
  into table-slot context and expected next targets.

---

## 1. TL;DR

1. **Entry chain (S, with native anchor M):**
   `0x00f568a1` -> `__security_init_cookie 0xf690d5` -> `jmp __tmainCRTStartup
   0xf56723` -> `_heap_init 0xf66bf0` -> CRT initializer tables -> `call WinMain
   0x69d1d0` at `0xf56831`.
2. **There are four real `_initterm` loop functions**, not the usual three:
   `0xf5897a` (error-stopping, two stack arguments, runs the 8-slot C
   initializer table), `0xf5895d` (plain loop, begin in `eax`, end on stack,
   runs the 11,661-slot C++ dynamic-initializer table), and `0xf69089` /
   `0xf690af` (fully linked loops over two **empty** ranges).
3. **The big C++ initializer table is at `0x106b464..0x1076a98`**
   (11,661 dwords; 10,912 non-NULL; 749 NULL). It is in `.rdata`; the targets
   are in `.text`. The section names `.CRT$XCU`/`.CRT$XT` do not exist in this
   PE; their contents were linked into `.rdata` and are identified by the loop
   that consumes them.
4. **TLS callbacks do not run at startup.** The TLS directory points at
   `0x1076ac4`; the array is `[0, 0, 0xf794ab, 0xf5b303, 0]`. The Windows
   loader stops at the first NULL, so zero callbacks run. The same two
   non-NULL pointers also appear in the CRT's exit-time `_initterm` range
   `0x1076ac8..0x1076ad4` (S; intent I).
5. **Measured frontier:** growth21/iter04 reached slot index 4870
   (`0x107007c` -> `0x00ff2ee0`) with `unknown indirect function`. The newer
   stages then measured `0xff2f80` (g22 iter01), `0xff3000` (g22 iter02),
   `0xff3240` (g22 iter03), `0xff3260` (g22 iter04), `0xff33c0` (g23 iter01),
   `0xff33f0` (g23 iter02) and **`0xff3660` (g23 iter03, index 4904)**. Every
   one of the 4,457 non-NULL slots before `0xff3660` returned. iter03's table
   expansion added `0xff3660`, `0xff3670`, `0xff3690` and `0xff3710`, so the
   next expected fault is **`0xff3720`** (index 4910). `0x00ff2ee0` has
   exactly one static pointer reference in the image - this table slot - and
   the fault reason is the indirect-call trap, so the failing call is the
   `_initterm` call at that slot.
6. **Remaining table work after `0x00ff3660`:** 6,454 non-NULL entries
   (6,472 after `0x00ff2ee0`). 98% of them sit in five dense runs
   (2,070 / 1,496 / 1,244 / 1,218 / 308). `tools/table_roots.py`'s default
   `--max-run 512` will cap those runs, so expect several growth iterations
   (or generate one explicit roots file). There are 659 distinct direct callees
   missing from the current slice; the first one appears at table index 5208
   (`0xff6270` calls `0x005ead40`).
7. **D3D9 is not reached yet.** The static path is
   `WinMain 0x69d1d0 -> 0x691030 -> 0xc6dbf0 -> 0xc6d510 -> 0xcd55e0 ->
   Direct3DCreate9(32) @ 0xcd5622`. A second factory is created at
   `0xcd1fef`. The first factory COM calls are the adapter enumeration:
   `GetAdapterCount` (slot 4, `0xce5e41`), `GetAdapterIdentifier` (slot 5,
   `0xce5a76`), `GetAdapterModeCount` (slot 6, `0xce5ab6`),
   `EnumAdapterModes` (slot 7, `0xce5ae4`) and `GetAdapterDisplayMode`
   (slot 8, `0xce5cc9`); `CreateDevice` (slot 16) follows at `0xcd8b72`.
8. **Shim readiness is good up to the adapter gate, but the first refusal is
   likely policy, not a missing import:** `CreateDevice` accepts only a
   400x240 windowed back buffer, and `GetDeviceCaps` honestly reports SM2.
   The game's renderer is built around SM3 (and, per `docs/ROADMAP.md`, a
   640x480 32-bpp mode). No fake success is possible or claimed.

---

## 2. PE facts (M)

`tools/init_trace.py pe`

| Item | Value |
|---|---|
| Image base | `0x00400000` |
| PE entry | `0x00f568a1` (`.text`) |
| `.text` | `0x00401000..0x0106a400` (raw end; VirtualSize ends `0x0106a31a`) |
| `.rdata` | `0x0106b000..0x0123b000` |
| `.data` | `0x0123b000..0x01bc2000` |
| `.tls` | `0x01bc2000..0x01bc4600` |
| TLS directory VA | `0x011b2028` |
| TLS raw data | `0x01bc2000..0x01bc4600` |
| TLS index VA | `0x01bbeb54` |
| **TLS callback array** | `0x01076ac4` -> `[0, 0, 0x00f794ab, 0x00f5b303, 0]` |
| Load-config VA | `0x011b1fd8`, size `0x48` |
| Security cookie | `0x012de484` (`__security_init_cookie` reads it at `0xf690dd`) |
| Import table | 15 DLLs, 266 imports |
| `d3d9.dll` static import | only `D3DPERF_SetOptions` (IAT `0x0106b3dc`) |
| `Direct3DCreate9` | **not imported**; resolved with `LoadLibraryA` + `GetProcAddress` |

The lack of a `msvcrt.dll` import in `TESV.exe` is important: the CRT is
statically linked into the executable. The `msvcrt.dll` trap names in the host
log (part of the 154 total unresolved imports) come from the other mapped
modules (binkw32 / d3dx9_42 / steam_api), not from `TESV.exe`.

---

## 3. Initializer mechanisms actually present

### 3.1 PE entry (`0x00f568a1`, M/S)

```
00f568a1  e8 2f280100   call 0x00f690d5      ; __security_init_cookie
00f568a6  e9 78feffff   jmp  0x00f56723      ; __tmainCRTStartup
```

`0x00f690d5` initializes the cookie from `[0x12de484]` and writes
`[0x12de488]` (the complement). This is the classic MSVC `__security_init_cookie`.

### 3.2 `_initterm` loop variants (S)

| Loop | Kind | Arguments | Consumes | Observed |
|---|---|---|---|---|
| `0x00f5897a` | error-stopping (`_initterm_e` shape) | begin/end on stack | `0x1076a9c..0x1076abc` | returns 0 |
| `0x00f5895d` | plain `_initterm` | begin in `eax`, end on stack | `0x106b464..0x1076a98` | walks the table |
| `0x00f69089` | fully linked (absolute bounds) | constants inside loop | `0x1237684..0x1237684` (empty) | zero calls |
| `0x00f690af` | fully linked (absolute bounds) | constants inside loop | `0x123768c..0x123768c` (empty) | zero calls |

`0xf5899e` is the `_cinit`-shaped caller. It calls:

```
0xf589c5  call 0xf5e757                 ; runtime/FP state
0xf589d4  call 0xf5897a                 ; _initterm_e(0x1076a9c, 0x1076abc)
0xf589e4  call 0xf520bc (atexit)        ; register the empty XT runner 0xf690af
0xf589f5  call 0xf5895d                 ; _initterm(0x106b464, 0x1076a98)
```

Two more call sites exist in the **exit path** function `0xf58a23`
(reached from `0xf58b4f/65/7b/8a`, which `__tmainCRTStartup` calls after
WinMain returns):

```
0xf58aef  _initterm(0x1076ac8, 0x1076ad4)   ; 3 slots, 2 non-NULL: 0xf794ab, 0xf5b303
0xf58aff  _initterm(0x1076ad8, 0x1076adc)   ; 1 slot, 0 non-NULL
```

Those two targets are exactly the two non-NULL TLS callback pointers. This is
measured as data (`0x1076ac8..0x1076ad4` contains those values); whether the
CRT intends this as the DLL-detach/TLS terminator table is **inference**.

### 3.3 Tables discovered from the loop code (M/S)

`tools/init_trace.py tables`

| Table | Base..end | Slots | Non-NULL | NULL | In growth21/iter04 | Claim |
|---|---|---:|---:|---:|---:|---|
| C initializers (`_initterm_e`) | `0x1076a9c..0x1076abc` | 8 | 7 | 1 | 7/7 | all passed |
| C++ dynamic initializers | `0x106b464..0x1076a98` | 11,661 | 10,912 | 749 | 4,439 | prefix passed to index 4870 |
| Exit-time table | `0x1076ac8..0x1076ad4` | 3 | 2 | 1 | 2/2 | not on startup path |
| Exit-time table | `0x1076ad8..0x1076adc` | 1 | 0 | 1 | - | empty |
| Fully linked loop | `0x1237684..0x1237684` | 0 | 0 | 0 | - | empty |
| Fully linked loop | `0x123768c..0x123768c` | 0 | 0 | 0 | - | empty |

Every non-NULL target in the two big tables is a discovered function start in
`analysis/tesv/functions.json`; the tools report `bad=0` for both.

Target classes in the 10,912 non-NULL C++ entries (heuristic from the first
bytes; all classes are diagnostic labels, not symbols):

| Class | Count |
|---|---:|
| `code` (ordinary initializer body) | 6,871 |
| `data_init` (small `fld/fmul/fstp`, `mov` initializer) | 3,306 |
| `jump_thunk` (`E9`/`E8` stub) | 384 |
| `atexit_thunk` (`push dtor; call 0xf520bc; pop ecx; ret`) | 340 |
| `static_guard` (`test byte [guard],1` + `atexit`) | 11 |

### 3.4 TLS callbacks: present, zero executed at startup (M)

The TLS directory at `0x011b2028` points `AddressOfCallBacks` at `0x01076ac4`.
The array in the image is:

```
0x01076ac4: 0x00000000
0x01076ac8: 0x00000000
0x01076acc: 0x00f794ab
0x01076ad0: 0x00f5b303
0x01076ad4: 0x00000000
```

The Windows loader stops at the first NULL, so **zero** TLS callbacks run.
This project's loader matches that rule and must not "fix" it by skipping
NULLs. The host/runtime line
`[win32] ran executable TLS callbacks (CRT thread data)` is the loader's own
TLS-contract log, not evidence that TESV's array ran.

Both non-NULL callbacks (`0xf794ab`, `0xf5b303`) are discovered functions and
are present in the current slice. They also appear in the CRT's exit-time
`_initterm` range `0x1076ac8..0x1076ad4` (M data; the intent is I).

### 3.5 No `.CRT$XC*` / `.CRT$XT*` sections

The PE section table has no `.CRT` section. The initializer/terminator data was
linked into `.rdata` (tables) and `.text` (the tiny initializer/thunk bodies).
The tables are therefore identified by their consumer (`_cinit`) and by their
position, not by a section name. This is a structural inference backed by the
exact code references above.

### 3.6 `atexit` / `_onexit` registration (M/S)

- `0xf520bc` is `atexit`: it pushes a function pointer, calls `0xf52080`, and
  returns 0 on success / -1 on failure. **340** table entries are direct
  `atexit` thunks. The dominant direct callee of the *remaining* entries is
  also `0xf520bc` (over 7,000 call sites in the first 128 bytes of each
  entry), so most of the remaining table is destructor registration, not
  engine initialization.
- `0xf52080` is the internal one-time-exit table for the twice-called state.
- `0xf51a01` is itself an initializer that registers `0xf519c1` through
  `atexit` and returns 0 or 0x18.
- Destructors are **not** stored in a separate `.data` pointer table that the
  startup loop walks. No second dtor-thunk array was found in `.data`; the only
  dtor dispatch is (a) `atexit` registration during each initializer and
  (b) the two exit-time `_initterm` ranges at `0x1076ac8` / `0x1076ad8`.

### 3.7 Dispatch / SEH thunks (S)

- `0xf5d120` / `0xf5d165` are the SEH prolog/epilog (`fs:[0]` chain, cookie
  XOR). They are not initializers but appear around every guarded CRT call.
- `0xf58b4f`, `0xf58b65`, `0xf58b7b`, `0xf58b8a` are `_exit`/`_cexit`-shaped
  wrappers around `0xf58a23`.
- 384 table entries are `jmp`/`call` stubs (`jump_thunk`), including the
  `0x103b5a2`-style entries that caused earlier table roots.
- The executable does not use MSVC lazy-ILT thunks. The D3DX lazy-ILT case
  documented in `docs/DEBUGGING.md` is in `D3DX9_42.dll`, not in this EXE.

### 3.8 What is *not* executed from the other mapped DLLs

`d3dx9_42.dll`, `binkw32.dll`, `X3DAudio1_7.dll` and `steam_api.dll` are mapped
as data-only modules. Their exports resolve, but their `DllMain`/CRT
initializers do not run and their code is not lifted. Therefore the only
initializer mechanism on this path is TESV's own linked CRT.

---

## 4. Ordered trace: PE entry -> WinMain

### 4.1 Measured anchors

- **M** Native Windows execution (`build_host/native_order4.txt`): a breakpoint
  at `0x01016d70` (initializer index 4) hits with stack
  `00f5896f 00000000 ... 00f589fa 01076a98 ... 00f5680a`, i.e. the call comes from
  the `_initterm` loop and returns into `_cinit` at `0xf589fa`.
- **M** Host run `analysis/growth21/iter04/probe_run.log`:
  `first fault: Guest fault at 00ff2ee0: unknown indirect function`.
  The `_initterm` loop reached slot 4870 and could not call its target.
  `0x00ff2ee0` has exactly one static pointer reference in the image - this
  table slot - and the fault reason is the indirect-call trap, so the failing
  indirect call is the `_initterm` call at this slot. Consequently **all 4,439
  non-NULL slots before slot 4870 returned**.
- **M** The completed growth stages measured the next slots in exactly table
  order:

  | Iteration | Fault | Table index | Table slot |
  |---|---|---:|---|
  | growth22/iter01 | `0xff2f80` | 4874 | `0x107008c` |
  | growth22/iter02 | `0xff3000` | 4883 | `0x10700b0` |
  | growth22/iter03 | `0xff3240` | 4890 | `0x10700cc` |
  | growth22/iter04 | `0xff3260` | 4893 | `0x10700d8` |
  | growth23/iter01 | `0xff33c0` | 4897 | `0x10700e8` |
  | growth23/iter02 | `0xff33f0` | 4900 | `0x10700f4` |
  | growth23/iter03 | **`0xff3660`** | **4904** | **`0x1070104`** |

  At growth23/iter03, 4,457 non-NULL slots preceded the fault and all 4,457
  were in the slice. This is a direct measurement of the walk order, not a
  reconstruction.
- **M** Native Windows entry stack (`build_host/native_order.txt`):
  `entry at 00f568a1`, `[mem]`-side TLS message from our loader.
- No WinMain body or D3D9 call has been executed by the lifted runtime yet.
  Everything below WinMain is **S** unless stated.

### 4.2 Stage table

`tools/init_trace.py trace --src 0xf568a1 --dst 0x69d1d0`

| # | Call site | Target | Role | Status |
|---:|---|---|---|---|
| 1 | `0xf568a1` | `0xf690d5` | `__security_init_cookie` | S; M via native entry stack |
| 2 | `0xf568a6` (jmp) | `0xf56723` | `__tmainCRTStartup` | S; M native |
| 3 | `0xf56791` | `0xf66bf0` | `_heap_init` (calls `HeapCreate`) | S; the heap exists in every measured run |
| 4 | `0xf567a3` | `0xf5d961` | module/command-line setup (uses `GetModuleHandleW`) | S |
| 5 | `0xf567b4` | `0xf69089` | `_initterm` over empty `0x1237684` | S; zero iterations |
| 6 | `0xf567bc` | `0xf68267` | STARTUPINFO/console/argv setup | S |
| 7 | `0xf567d8` | `0xf68f52` | environment strings (`GetEnvironmentStringsW`) | S |
| 8 | `0xf567e2` | `0xf68e97` | environment/Unicode setup | S |
| 9 | `0xf567f3` | `0xf68c1f` | environment array copy | S |
| 10 | `0xf56805` | `0xf5899e` | `_cinit` (the four table calls above) | S; M native stack |
| 11 | `0xf56816` | `0xf68bc0` | command-line parse, returns `lpCmdLine` in `eax` | S |
| 12 | **`0xf56831`** | **`0x69d1d0`** | **WinMain(hInstance=0x400000, hPrev=0, lpCmdLine, nCmdShow)** | S |
| 13 | `0xf5683f` | `0xf58b4f` | post-WinMain exit path | S |
| 14 | `0xf56844` | `0xf58b7b` | post-WinMain exit path | S |

The WinMain call is a direct `E8` call with `push 0x400000` (hInstance),
`push 0` (hPrevInstance), `push eax` (lpCmdLine), `push ecx` (`nCmdShow`;
`0xa` = `SW_SHOWDEFAULT` when STARTUPINFO has no `wShowWindow`). There is no
`__getmainargs` indirect dispatch on this path.

### 4.3 C initializer table (`_initterm_e`, `0x1076a9c..0x1076abc`)

`tools/init_trace.py list --range 0x1076a9c:0x1076abc`

| Index | Slot | Target | In slice | Class | Observed |
|---:|---|---|---|---|---|
| 0 | `0x1076a9c` | NULL | - | guard | skipped |
| 1 | `0x1076aa0` | `0x00f5204f` | yes | code | passed (M, prefix) |
| 2 | `0x1076aa4` | `0x00f5b252` | yes | data_init | passed (M, prefix) |
| 3 | `0x1076aa8` | `0x00f5e9a2` | yes | jump_thunk | passed |
| 4 | `0x1076aac` | `0x00f5f074` | yes | code | passed |
| 5 | `0x1076ab0` | `0x00f62c61` | yes | code | passed |
| 6 | `0x1076ab4` | `0x00f51a01` | yes | atexit_thunk | passed |
| 7 | `0x1076ab8` | `0x00f68a51` | yes | code | passed |

Role labels (I, from behavior):

- `0xf5204f` allocates a 32-byte block and stores it in two globals
  (`0x1bc148c`, `0x1bc1488`) - CRT lock/TLS table setup.
- `0xf5b252` allocates the stdio file table (0x200 entries) - `_ioinit`.
- `0xf5e9a2`, `0xf62c61` call `0xf5e940` and store the result - argv/environment
  conversion.
- `0xf5f074` sets `[0x1bc1490]` and calls `0xf5eeda(-3)` - file-mode/app-type
  setup.
- `0xf51a01` registers `0xf519c1` via `atexit`.
- `0xf68a51` calls `_set_invalid_parameter_handler(0xf68a0f)`.

The error-stopping semantics are real: `0xf5897a` exits the loop when a
callee's `eax` is non-zero, and `_cinit` branches to its failure path. In the
measured runs all seven return zero.

### 4.4 C++ dynamic initializer table (`_initterm`, `0x106b464..0x1076a98`)

First slots (`tools/init_trace.py list --range 0x106b464:0x106b4a4`):

| Index | Slot | Target | In slice | Class | Status |
|---:|---|---|---|---|---|
| 0 | `0x106b464` | NULL | - | guard | skipped |
| 1 | `0x106b468` | `0x0103b56a` | yes | code | passed (M prefix) |
| 2 | `0x106b46c` | `0x0103b58c` | yes | code | passed |
| 3 | `0x106b470` | `0x0103b5a2` | yes | atexit_thunk | passed |
| 4 | `0x106b474` | `0x01016d70` | yes | code | passed; **M** native breakpoint |
| 5 | `0x106b478` | `0x00fc3c70` | yes | code | passed |
| 6 | `0x106b47c` | `0x00fc3ca0` | yes | code | passed |
| 7 | `0x106b480` | `0x00fc3cd0` | yes | code | passed |

Frontier region (`tools/init_trace.py list --range 0x1070070:0x1070110 --table-base 0x106b464`):

| Index | Slot | Target | growth21/iter04 | growth23/iter03 | Class | Measured status |
|---:|---|---|---|---|---|---|
| 4867 | `0x1070070` | `0x00ff2ed0` | yes | yes | atexit_thunk | passed |
| 4868 | `0x1070074` | NULL | - | - | guard | skipped |
| 4869 | `0x1070078` | NULL | - | - | guard | skipped |
| **4870** | **`0x107007c`** | **`0x00ff2ee0`** | **no** | **yes** | atexit_thunk | **fault in growth21/iter04** |
| 4871 | `0x1070080` | `0x00ff2ef0` | no | yes | static_guard | passed |
| 4872 | `0x1070084` | `0x00ff2f70` | no | yes | atexit_thunk | passed |
| 4873 | `0x1070088` | NULL | - | - | guard | skipped |
| **4874** | **`0x107008c`** | **`0x00ff2f80`** | no | yes | data_init | **fault in g22/iter01** |
| 4875 | `0x1070090` | `0x00ff2fa0` | no | yes | data_init | passed |
| 4876 | `0x1070094` | `0x00ff2fc0` | no | yes | data_init | passed |
| 4877 | `0x1070098` | `0x00ff2fd0` | no | yes | data_init | passed |
| 4878 | `0x107009c` | `0x00ff2ff0` | no | yes | atexit_thunk | passed |
| **4883** | **`0x10700b0`** | **`0x00ff3000`** | no | yes | code | **fault in g22/iter02** |
| 4884 | `0x10700b4` | `0x00ff3230` | no | yes | atexit_thunk | passed |
| **4890** | **`0x10700cc`** | **`0x00ff3240`** | no | yes | data_init | **fault in g22/iter03** |
| 4891 | `0x10700d0` | `0x00ff3250` | no | yes | atexit_thunk | passed |
| **4893** | **`0x10700d8`** | **`0x00ff3260`** | no | yes | static_guard | **fault in g22/iter04** |
| 4894 | `0x10700dc` | `0x00ff33b0` | no | yes | atexit_thunk | passed |
| **4897** | **`0x10700e8`** | **`0x00ff33c0`** | no | yes | data_init | **fault in g23/iter01** |
| 4898 | `0x10700ec` | `0x00ff33e0` | no | yes | atexit_thunk | passed |
| **4900** | **`0x10700f4`** | **`0x00ff33f0`** | no | yes | code | **fault in g23/iter02** |
| 4901 | `0x10700f8` | `0x00ff3650` | no | yes | atexit_thunk | passed |
| **4904** | **`0x1070104`** | **`0x00ff3660`** | no | **no** | atexit_thunk | **fault in g23/iter03** |
| 4905 | `0x1070108` | `0x00ff3670` | no | no | data_init | in iter03 roots |
| 4906 | `0x107010c` | `0x00ff3690` | no | no | code | in iter03 roots |
| 4907 | `0x1070110` | `0x00ff3710` | no | no | atexit_thunk | in iter03 roots |
| 4910 | `0x107011c` | `0x00ff3720` | no | no | atexit_thunk | expected next fault |

`0x00ff2ee0` decodes as:

```
00ff2ee0  68 a0050501   push 0x010505a0      ; destructor
00ff2ee5  e8 d2f1f5ff   call 0x00f520bc      ; atexit
00ff2eea  59            pop ecx
00ff2eeb  c3            ret
```

`0x00ff2ef0` is a static-initialization guard: it tests
`[0x1b209a0]`, sets the guard, sets a vtable pointer and registers two
destructors (`0x010505c0`, `0x010504a0`) through `atexit`. `0x00ff2f70` is
another single-`atexit` thunk (`push 0x10505b0`).

This is why the frontier is now a stream of tiny `.text`-tail initializers, not
ordinary engine functions.

The full per-entry enumeration is in
`E:\Skyrim_N3DS_build\init_trace\table_xc_growth23_iter03.csv` (11,661 rows;
columns `index,slot,target,region,is_function_start,in_slice,class`). The
observed / expected status for every row follows from the measured faults:

| Row range | Status |
|---|---|
| index 0 (NULL) | skipped |
| indices 1..4903, non-NULL | **observed returned** (M: seven measured walks) |
| index 4904 (`0xff3660`) | **observed first fault in growth23/iter03** (M) |
| indices 4905..4907 | in iter03 roots; expected pass in the next stage |
| indices 4910..end, non-NULL | expected future call (S) - not in the current slice |

### 4.5 WinMain -> renderer gate (S)

`0x69d1d0` has exactly one direct caller (`0xf56831`). Inside WinMain:

| Call site | Target | Role |
|---|---|---|
| `0x69d1da` | `ole32.dll!CoInitialize` (IAT) | COM |
| `0x69d87e` | `USER32.dll!CreateWindowExA` | main window |
| `0x69d892` | `0xa48d60` | engine allocator |
| `0x69d89f` | `0x69bfe0` | window/device init helper |
| **`0x69d905`** | **`0x691030`** | **renderer object init (vtable method)** |
| `0x69d8e6` | `0x6ebc10` | engine stage |
| `0x69d8f4` | `0xa5b620` | engine stage |

`0x691030` (S):

```
00691030  mov eax,[ecx+0x10]
00691033  mov ecx,[ecx+0x0c]
00691036  sub esp,0x200
0069103c  push eax
0069103d  push ecx
0069103e  call 0xc6dbf0           ; renderer initialization gate
00691043  add esp,8
00691046  test eax,eax
00691048  jne 0x69107d
0069104a  push 0x1ba72e8 ...      ; "Failed to initialize renderer"
0069106f  call USER32!MessageBoxA
00691077  call KERNEL32!ExitProcess(0)
```

`0xc6dbf0` has exactly one direct caller (`0x69103e`). Its failure path is an
explicit MessageBox + `ExitProcess(0)`, not a trap.

---

## 5. Remaining initializer frontier after `0x00ff2ee0`

`tools/init_trace.py frontier --table 0x106b464:0x1076a98 --after 0xff2ee0`

### 5.1 Arithmetic (M/S)

| Quantity | Value |
|---|---:|
| Slots in table | 11,661 |
| Non-NULL | 10,912 |
| Non-NULL in growth21/iter04 | 4,439 |
| Non-NULL in growth23/iter03 | 4,457 |
| Remaining non-NULL after `0x00ff2ee0` | **6,472** |
| Remaining non-NULL after `0x00ff3660` | **6,454** |
| Maximal runs remaining after `0x00ff3660` | **47** |

Run-size distribution after `0x00ff3660`:

```
{2070:1, 1496:1, 1244:1, 1218:1, 308:1,
 14:1, 7:1, 6:4, 5:4, 3:6, 2:9, 1:17}
```

Five dense runs contain 6,336 entries (98% of the remaining total). This
matters because `tools/table_roots.py` expands only a maximal run and defaults
to `--max-run 512`: a single fault inside the 2,070-entry run adds at most 512
roots. Either raise `--max-run` for this region or emit one explicit roots file
with `--emit-roots`.

### 5.2 Exact next expected targets

After the original measured fault (`0x00ff2ee0`), the sequence was predicted
and then measured as follows:

| Index | Slot | Target | Class | Measured outcome |
|---:|---|---|---|---|
| 4871 | `0x1070080` | `0x00ff2ef0` | static_guard | passed before iter01 fault |
| 4872 | `0x1070084` | `0x00ff2f70` | atexit_thunk | passed |
| 4874 | `0x107008c` | `0x00ff2f80` | data_init | **fault, growth22/iter01** |
| 4875 | `0x1070090` | `0x00ff2fa0` | data_init | passed |
| 4876 | `0x1070094` | `0x00ff2fc0` | data_init | passed |
| 4877 | `0x1070098` | `0x00ff2fd0` | data_init | passed |
| 4878 | `0x107009c` | `0x00ff2ff0` | atexit_thunk | passed |
| 4883 | `0x10700b0` | `0x00ff3000` | code | **fault, growth22/iter02** |
| 4884 | `0x10700b4` | `0x00ff3230` | atexit_thunk | passed |
| 4890 | `0x10700cc` | `0x00ff3240` | data_init | **fault, growth22/iter03** |
| 4891 | `0x10700d0` | `0x00ff3250` | atexit_thunk | passed |
| 4893 | `0x10700d8` | `0x00ff3260` | static_guard | **fault, g22/iter04** |
| 4894 | `0x10700dc` | `0x00ff33b0` | atexit_thunk | passed |
| 4897 | `0x10700e8` | `0x00ff33c0` | data_init | **fault, g23/iter01** |
| 4898 | `0x10700ec` | `0x00ff33e0` | atexit_thunk | passed |
| 4900 | `0x10700f4` | `0x00ff33f0` | code | **fault, g23/iter02** |
| 4901 | `0x10700f8` | `0x00ff3650` | atexit_thunk | passed |
| 4904 | `0x1070104` | `0x00ff3660` | atexit_thunk | **fault, g23/iter03** |

Current next targets after `0x00ff3660` (S). growth23/iter03's table expansion
added `0xff3660`, `0xff3670`, `0xff3690` and `0xff3710`, so the next expected
fault is **`0xff3720`** (index 4910):

| Index | Slot | Target | Class |
|---:|---|---|---|
| 4905 | `0x1070108` | `0x00ff3670` | data_init (in iter03 roots) |
| 4906 | `0x107010c` | `0x00ff3690` | code (in iter03 roots) |
| 4907 | `0x1070110` | `0x00ff3710` | atexit_thunk (in iter03 roots) |
| 4910 | `0x107011c` | `0x00ff3720` | atexit_thunk (next expected fault) |
| 4913 | `0x1070128` | `0x00ff3730` | data_init |
| 4914 | `0x107012c` | `0x00ff3750` | data_init |
| 4915 | `0x1070130` | `0x00ff3770` | data_init |
| 4916 | `0x1070134` | `0x00ff3780` | data_init |
| 4917 | `0x1070138` | `0x00ff37a0` | atexit_thunk |
| 4922 | `0x107014c` | `0x00ff37b0` | static_guard |

### 5.3 Callee frontier inside the remaining table (S)

The table entries themselves are cheap; their callees are the real closure.
The tool reports **659 distinct direct callees that are function starts but
are absent from growth21/iter04**. This is an approximate scan over the first
128 bytes of each remaining entry (raw `E8`, filtered to discovered function
starts): it can miss calls later in a large function and can include a few
false positives. Use it as a ranked lead list, not as a complete call graph.
The largest:

| Missing callee | References | First slot (index) |
|---|---:|---|
| `0x00d6a8a0` | 1,351 | `0x1074ad8` (9629) -> `0x01023b80` |
| `0x00662800` | 174 | `0x1070d28` (5681) -> `0x00ffa2e0` |
| `0x008af3b0` | 103 | `0x10730bc` (7958) -> `0x01014b40` |
| `0x00a5de30` | 98 | `0x10724a4` (7184) -> `0x0100b180` |
| `0x00a5d2a0` | 83 | `0x10724a8` (7185) -> `0x0100b1a0` |
| `0x007d4b50` | 75 | `0x1071bd8` (6621) -> `0x01005cd0` |
| `0x00b7a950` | 49 | `0x10718b8` (6421) -> `0x01002f50` |
| `0x00953090` | 32 | `0x107349c` (8206) -> `0x01016b90` |
| `0x005ead40` | 30 | `0x10705c4` (5208) -> `0x00ff6270` |
| `0x007aca50` | 30 | `0x1071d18` (6701) -> `0x01006c70` |

**Earliest missing callee:** index **5208** (`0x10705c4` -> `0x00ff6270` calls
`0x005ead40`). That is 304 non-NULL entries after the current frontier
(4904); the entries between `0xff3660` and `0xff6270` call only functions
already in the slice. So the practical next sequence is:

1. add the next run(s) of table entries (through `0xff6270` at index 5208);
2. at that point add `0x005ead40` and its closure;
3. continue the table walk; `0x00d6a8a0` (index 9629) is the largest single
   future callee, referenced 1,351 times, so it will dominate the last third
   of the table.

To write **all** 659 missing callees (not only the printed top 10) in the same
one-address-per-line roots format as `--emit-roots`:

```powershell
.venv\Scripts\python.exe tools/init_trace.py frontier `
  --table 0x106b464:0x1076a98 --after 0xff3660 `
  --slice E:\Skyrim_N3DS_build\analysis\growth23\iter03\slice_functions.json `
  --emit-callees E:\Skyrim_N3DS_build\analysis\init_callee_roots_after_ff3660.txt
```

That run writes 659 unique `0x%08x` lines. The table targets themselves are a
separate list: use `--emit-roots` for those. Both formats are accepted by
`slice_pe.py --extra-root-file`; do not overwrite a running supervisor's root
file with either without the owner's decision.

### 5.4 What "done" means for this table

When the loop reaches `0x1076a98`, `_cinit` returns to
`__tmainCRTStartup` at `0xf5680a`, then the startup code calls WinMain at
`0xf56831`. WinMain itself is already in the slice. Therefore the next major
barrier after the table is **not another initializer table**; it is the engine
code WinMain calls, and then the renderer gate `0x691030 -> 0xc6dbf0`.

This is an estimate, not a guarantee. A useful scale estimate for the next
slice is:

| Component | Functions |
|---|---:|
| Remaining non-NULL table targets after `0xff3660` | 6,454 |
| Distinct direct callees of those targets missing from the slice | 659 |
| **Union (the two sets are disjoint in this scan)** | **7,113** |

The callee count is approximate (first 128 bytes only), and transitive callees
can add more. Treat 7,113 as a scale estimate, not a promise that this many
functions finish startup.

---

## 6. D3D9 path

### 6.1 Dynamic loading (S)

`Direct3DCreate9` is resolved at runtime; the only static `d3d9.dll` import is
`D3DPERF_SetOptions`.

`0xcd1f70` (probe helper) and `0xcd1fb0` / `0xcd55e0` (factories) all share:

```
push 0x01158294                 ; "d3d9.dll"
call LoadLibraryA               ; IAT 0x0106b0b4
mov  [0x01bae440], eax
push 0x01158284                 ; "Direct3DCreate9"
push eax
call GetProcAddress             ; IAT 0x0106b0b0
mov  [0x01bae448], eax
...
push 0x20                       ; D3D_SDK_VERSION = 32
call eax                        ; Direct3DCreate9
```

Measured call sites:

| Site | Context | What |
|---|---|---|
| `0xcd1fc9` | `0xcd1fb0` | `LoadLibraryA("d3d9.dll")` |
| `0xcd1fde` | `0xcd1fb0` | `GetProcAddress("Direct3DCreate9")` |
| **`0xcd1fef`** | `0xcd1fb0` | **`Direct3DCreate9(32)`**, stores factory at `0x1bae444` |
| `0xcd55fb` | `0xcd55e0` | `LoadLibraryA("d3d9.dll")` |
| `0xcd5610` | `0xcd55e0` | `GetProcAddress("Direct3DCreate9")` |
| **`0xcd5622`** | `0xcd55e0` | **`Direct3DCreate9(32)`**, wraps it via `0xce5ed0` |

### 6.2 Renderer ordering (S)

`0xc6dbf0` calls, in order:

| Call site | Target | Meaning |
|---|---|---|
| `0xc6dc97` | `0xc6d510` | pre-init / adapter probe |
| `0xc6d567` (inside `0xc6d510`) | `0xcd55e0` | **first factory creation** (`Direct3DCreate9` at `0xcd5622`) |
| `0xc6dcf1` | `USER32!CreateWindowExA` | renderer window |
| `0xc6dd1b/2b/37/66` | GDI/USER | class/rect/position |
| `0xc6dddb` | `0xcdb620` | second factory + device wrapper |
| `0xc6de65` | factory slot 11 | `CheckDeviceMultiSampleType` (conditional) |
| `0xc6dfa6` | `0xcd55e0` | returns the cached first factory |
| `0xc6e0a7` | factory slot 10 | `CheckDeviceFormat` (usage `0x80000`, format `0x71` = `A16B16G16R16F`) |
| `0xc6e2cc` | `0xc6cc40` | renderer capability stage |
| `0xc6e302` | `0xc72350` | renderer capability stage |

Inside `0xcd55e0`, the raw factory is wrapped immediately:

```
0xcd5622  call eax                  ; Direct3DCreate9(32), raw IDirect3D9*
0xcd5646  call 0xce5ed0             ; wrap raw factory
0xce5f04  call 0xce5e30             ; enumerate adapters
0xce5e41  call [vtable+0x10]        ; slot 4  = GetAdapterCount
0xce5e4b  call 0xce5f60             ; allocate per-adapter records
0xce5e76  call 0xce5a10             ; per-adapter setup
0xce5a76  call [vtable+0x14]        ; slot 5  = GetAdapterIdentifier
0xce5ab6  call [vtable+0x18]        ; slot 6  = GetAdapterModeCount
0xce5ae4  call [vtable+0x1c]        ; slot 7  = EnumAdapterModes
0xce5cc9  call [vtable+0x20]        ; slot 8  = GetAdapterDisplayMode
```

So the first *D3D9 COM* calls are the adapter enumeration calls, not
`CreateDevice`. `0xce5a10` also allocates adapter records through the engine
allocator (`0xa48d60`). The exact slot/call list is reproducible with
`tools/init_trace.py slots` (output in `init_trace/com_slots.txt`):

```
== 00ce5e30..00ce5ecf
  slot  4 (0x10)  load @ 00ce5e3d  call @ 00ce5e41  [ecx]
== 00ce5a10..00ce5d53
  slot  5 (0x14)  load @ 00ce5a6f  call @ 00ce5a76  [ecx]
  slot  6 (0x18)  load @ 00ce5ab0  call @ 00ce5ab6  [ecx]
  slot  7 (0x1c)  load @ 00ce5ad6  call @ 00ce5ae4  [edx]
  slot  8 (0x20)  load @ 00ce5cbf  call @ 00ce5cc9  [eax]
```

Inside `0xcdb620`:

```
0xcdb620 call 0xcd1fb0          ; second Direct3DCreate9 -> 0x1bae444
0xcdb6c0 call 0xcd8890          ; device creation
```

Inside `0xcd8890` the first factory COM call on the **second** factory is:

```
0xcd8b42  mov  eax,[0x1bae444]  ; IDirect3D9*
0xcd8b47  mov  ecx,[eax]        ; vtable
...
0xcd8b6f  mov  eax,[ecx+0x40]   ; slot 16 = CreateDevice
0xcd8b72  call eax
```

`0xcd8c04` is the retry call with the same slot. `0xc6de65` uses
`[vtable+0x2c]` (slot 11, `CheckDeviceMultiSampleType`); `0xc6e0a7` uses
`[vtable+0x28]` (slot 10, `CheckDeviceFormat`).

No D3D9 call has been executed by the lifted runtime. The ordering above is
decoded, not observed.

### 6.3 Shim readiness vs missing

The D3D9 bridge is `source/d3d9_com.c` + `source/d3d9_adapter.c`; the host run
registers the null backend, the 3DS run registers the PICA backend.

| Surface | State | Detail |
|---|---|---|
| `LoadLibraryA("d3d9.dll")` | **ready** | `win32_shims.c` returns `D3D_HANDLE`; `d3d9_com` installs an external resolver |
| `GetProcAddress("Direct3DCreate9")` | **ready** | `d3d9_com_resolve` |
| `Direct3DCreate9(32)` | **ready** | returns a guest FACTORY object |
| `GetAdapterCount` | **ready** | 1 |
| `GetAdapterIdentifier` | **ready** | software bridge identity, zero vendor/device/WHQL |
| `GetAdapterModeCount` / `EnumAdapterModes` | **partial** | one mode: 400x240 X8R8G8B8, 60 Hz |
| `GetAdapterDisplayMode` | **ready** | 400x240 X8R8G8B8 |
| `CheckDeviceType` | **ready** | HAL, windowed, X8R8G8B8/A8R8G8B8 |
| `CheckDeviceFormat` | **partial** | X8R8G8B8/A8R8G8B8/DXT1/3/5 with `QUERY_FILTER`/`QUERY_WRAPANDMIP`; rejects `A16B16G16R16F` (the `0x71` probe), depth formats, auto-mip and most usage bits |
| `CheckDeviceMultiSampleType` | **partial** | only "none" |
| `CheckDepthStencilMatch` | **missing** | always `D3DERR_NOTAVAILABLE` (no depth-stencil surfaces) |
| `GetDeviceCaps` | **ready, honest** | VS/PS 2.0 only; 1 texture stage; 4 VS constants; 1024^2 textures |
| `CreateDevice` | **ready with backend** | host null backend / 3DS PICA backend; **only 400x240 windowed**, one back buffer, no MSAA, `D3DSWAPEFFECT_DISCARD`, no auto depth-stencil, no flags, interval <= 1 |
| Device methods | **partial** | implemented subset: Reset, Present, Clear, Begin/EndScene, viewport, render states, sampler, texture, VB/IB, FVF/decl, stream/indices, VS/PS create/set, DrawPrimitive/DrawIndexed; every other slot is `unsupported D3D9 COM method` |
| `D3DPERF_SetOptions` | **ready** | static import shim |
| D3DX9_42 exports | **resolved but not lifted** | 18 imports resolve from the staged DLL, but their bodies are not lifted; a call traps at the DLL address |
| SM3 shaders | **architectural gap** | PICA200 has no programmable fragment shader; the current TEV translator covers 19 real PS chunks plus a separate menu-light prototype |

Expected first refusal points (S/I, in order):

1. If the game asks `CreateDevice` for a back buffer other than 400x240, the
   bridge returns `D3DERR_UNAVAILABLE` with the log
   `back buffer is not the 400x240 display mode`. The renderer gate then shows
   "Failed to initialize renderer" and exits.
2. If the prepared 400x240 INIs make the game ask for 400x240, `CreateDevice`
   can pass. The renderer then probes `CheckDeviceFormat` for
   `A16B16G16R16F` with usage `0x80000` (`D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING`,
   not SRGBREAD; `D3DUSAGE_QUERY_SRGBREAD` is `0x00010000` and is handled as a
   separate fallback probe), and for
   the `ATOC` / `RESZ` four-character formats (`0xc6e26b`, `0xc6e293`,
   `0xc6e2c0`, `0xc6e328`, `0xc6e445`). The bridge rejects all of these: it
   accepts only X8R8G8B8/A8R8G8B8/DXT1/3/5 with `QUERY_FILTER` or
   `QUERY_WRAPANDMIP`. The next honest refusal is therefore either those
   capability probes or the SM2 `GetDeviceCaps` answer (the game expects SM3).
3. If the renderer reaches D3DX or an unimplemented device method before that,
   the runtime traps with the exact COM method name or D3DX import; it does not
   return a fake success.

The prepared INIs (`docs/STATUS.md`, `docs/ROADMAP.md`) request 400x240, so
case 2 is the realistic first path, not case 1. Neither has been measured yet.

---

## 7. Reproduce

All commands assume the repository root and the project venv. Outputs used for
this document are under `E:\Skyrim_N3DS_build\init_trace\`.

```powershell
# PE entry, TLS directory, load config
.venv\Scripts\python.exe tools/init_trace.py pe `
  --exe E:\skyr\TESV.exe *> E:\Skyrim_N3DS_build\init_trace\pe.txt

# Discover and verify every initializer table from the loop code
.venv\Scripts\python.exe tools/init_trace.py tables `
  --exe E:\skyr\TESV.exe `
  --functions E:\Skyrim_N3DS_build\analysis\tesv\functions.json `
  --slice E:\Skyrim_N3DS_build\analysis\growth21\iter04\slice_functions.json `
  --json E:\Skyrim_N3DS_build\init_trace\init_tables.json

# Entry -> WinMain bounded direct-call trace
.venv\Scripts\python.exe tools/init_trace.py trace `
  --src 0xf568a1 --dst 0x69d1d0 --depth 3 --per-function 20 `
  --slice E:\Skyrim_N3DS_build\analysis\growth21\iter04\slice_functions.json `
  *> E:\Skyrim_N3DS_build\init_trace\trace_entry_winmain.txt

# Frontier after the measured fault (stable and newest measured slices)
.venv\Scripts\python.exe tools/init_trace.py frontier `
  --table 0x106b464:0x1076a98 --after 0xff2ee0 --count 12 `
  --slice E:\Skyrim_N3DS_build\analysis\growth21\iter04\slice_functions.json `
  *> E:\Skyrim_N3DS_build\init_trace\frontier_growth21_iter04.txt

.venv\Scripts\python.exe tools/init_trace.py frontier `
  --table 0x106b464:0x1076a98 --after 0xff3660 --count 16 `
  --slice E:\Skyrim_N3DS_build\analysis\growth23\iter03\slice_functions.json `
  *> E:\Skyrim_N3DS_build\init_trace\frontier_growth23_iter03_after_ff3660.txt

# Optional: emit one roots file for the whole remaining table (do not feed it
# to a running supervisor without the owner's decision; it changes the slice).
.venv\Scripts\python.exe tools/init_trace.py frontier `
  --table 0x106b464:0x1076a98 --after 0xff2ee0 `
  --slice E:\Skyrim_N3DS_build\analysis\growth23\iter03\slice_functions.json `
  --emit-roots E:\Skyrim_N3DS_build\init_trace\roots_after_ff3660.txt

# Bounded table dumps
.venv\Scripts\python.exe tools/init_trace.py list `
  --range 0x1076a9c:0x1076abc --show-null `
  --slice E:\Skyrim_N3DS_build\analysis\growth21\iter04\slice_functions.json

.venv\Scripts\python.exe tools/init_trace.py list `
  --range 0x1070070:0x1070090 --table-base 0x106b464 --show-null `
  --slice E:\Skyrim_N3DS_build\analysis\growth21\iter04\slice_functions.json

# Full per-slot enumeration (bounded file, not a terminal dump)
.venv\Scripts\python.exe tools/init_trace.py list `
  --range 0x106b464:0x1076a98 --table-base 0x106b464 --csv `
  --slice E:\Skyrim_N3DS_build\analysis\growth21\iter04\slice_functions.json `
  > E:\Skyrim_N3DS_build\init_trace\table_xc_growth21.csv

.venv\Scripts\python.exe tools/init_trace.py list `
  --range 0x106b464:0x1076a98 --table-base 0x106b464 --csv `
  --slice E:\Skyrim_N3DS_build\analysis\growth23\iter03\slice_functions.json `
  > E:\Skyrim_N3DS_build\init_trace\table_xc_growth23_iter03.csv

.venv\Scripts\python.exe tools/init_trace.py list `
  --range 0x1076a9c:0x1076abc --table-base 0x1076a9c --csv `
  --slice E:\Skyrim_N3DS_build\analysis\growth21\iter04\slice_functions.json `
  > E:\Skyrim_N3DS_build\init_trace\table_xi_growth21.csv

# D3D9 dynamic resolution and call sites
.venv\Scripts\python.exe tools/init_trace.py d3d9 `
  --exe E:\skyr\TESV.exe --renderer 0xc6dbf0 `
  *> E:\Skyrim_N3DS_build\init_trace\d3d9_call_sites.txt

# COM vtable slot calls in the renderer/enumeration helpers (heuristic)
.venv\Scripts\python.exe tools/init_trace.py slots `
  --range 0xce5e30:0xce5ecf --range 0xce5a10:0xce5d53 `
  --range 0xcd8890:0xcd8f1b --range 0xc6dbf0:0xc6e908 `
  *> E:\Skyrim_N3DS_build\init_trace\com_slots.txt

# Annotate a saved host run (no game launch, no runtime change)
.venv\Scripts\python.exe tools/annotate_host_run.py `
  E:\Skyrim_N3DS_build\analysis\growth23\iter03 `
  --slice E:\Skyrim_N3DS_build\analysis\growth23\iter03\slice_functions.json `
  --markdown E:\Skyrim_N3DS_build\init_trace\growth23_iter03.md `
  --json E:\Skyrim_N3DS_build\init_trace\growth23_iter03.json
```

Tool notes:

- `tools/init_trace.py` identifies the four loops by their emitter signatures
  (`8bff558bec568b750833c0eb`, `8bff558bec568bf0eb`, and the two
  `8bff56b8...578bf83bc6` absolute loops), then recovers the table bounds from
  the immediate arguments. If the binary changes, the tool reports no table
  rather than inventing one.
- `tools/annotate_host_run.py` treats the fault address as the first failure
  and checks that every non-NULL slot before it is present in the supplied
  slice. A mismatch is reported as a warning, not silently accepted.
- No runtime trace overlay was added. `rt_prof` records hit counts, not call
  order, and the runner has no ordered trace buffer; adding one would require
  editing runtime dispatch, which was explicitly out of scope. A future overlay
  must live under `tools/`, be non-production and be clearly marked as such.

---

## 8. Evidence files

| Path | What |
|---|---|
| `E:\Skyrim_N3DS_build\init_trace\init_tables.json` | discovered tables, counts, classes |
| `E:\Skyrim_N3DS_build\init_trace\pe.txt` | PE entry/TLS/load-config/section dump |
| `E:\Skyrim_N3DS_build\init_trace\trace_entry_winmain.txt` | ordered entry->WinMain trace |
| `E:\Skyrim_N3DS_build\init_trace\frontier_growth21_iter04.txt` | frontier for the stable slice |
| `E:\Skyrim_N3DS_build\init_trace\frontier_growth23_iter03_after_ff3660.txt` | frontier for the newest measured slice |
| `E:\Skyrim_N3DS_build\init_trace\list_xi.txt` | full C initializer table |
| `E:\Skyrim_N3DS_build\init_trace\list_xc_first.txt` | first C++ initializer slots |
| `E:\Skyrim_N3DS_build\init_trace\list_frontier21.txt` | measured fault neighbourhood (0xff2ee0) |
| `E:\Skyrim_N3DS_build\init_trace\list_frontier22_iter04.txt` | measured fault neighbourhood (0xff3260) |
| `E:\Skyrim_N3DS_build\init_trace\d3d9_call_sites.txt` | Direct3DCreate9 / renderer call sites |
| `E:\Skyrim_N3DS_build\init_trace\com_slots.txt` | factory/device COM slot calls |
| `E:\Skyrim_N3DS_build\init_trace\table_xc_growth21.csv` | **all 11,661 C++ table slots** (stable slice) |
| `E:\Skyrim_N3DS_build\init_trace\table_xc_growth23_iter03.csv` | same table against the newest measured slice |
| `E:\Skyrim_N3DS_build\analysis\init_callee_roots_after_ff3660.txt` | 659 distinct missing direct callees after `0xff3660` (roots format) |
| `E:\Skyrim_N3DS_build\init_trace\table_xi_growth21.csv` | all 8 C initializer slots |
| `E:\Skyrim_N3DS_build\init_trace\growth23_iter03.md` | annotated measured run at `0xff3660` |
| `E:\Skyrim_N3DS_build\init_trace\growth23_iter03.json` | machine-readable annotation |
| `E:\Skyrim_N3DS_build\init_trace\growth21_iter04.md` | annotated host run |
| `E:\Skyrim_N3DS_build\init_trace\growth21_iter04.json` | machine-readable annotation |
| `E:\Skyrim_N3DS_build\analysis\growth21\iter04\probe_run.log` | measured first fault |
| `E:\Skyrim_N3DS_build\host\native_order4.txt` | native Windows initializer stack |

---

## 9. Measured vs inferred ledger

| Claim | Class | Evidence |
|---|---|---|
| PE entry is `0xf568a1`, calls `0xf690d5`, jumps to `0xf56723` | **M/S** | PE header + decode; native entry stack |
| WinMain is `0x69d1d0`, called from `0xf56831` | **S** | one `E8` call site; argument pushes decode |
| `_cinit` runs `0x1076a9c..0x1076abc` and `0x106b464..0x1076a98` | **S** | `0xf5899e` decode |
| XI table entries all returned in the measured run | **M** | fault occurs after the XI loop |
| 4,439 non-NULL C++ entries before slot 4870 returned | **M** | fault at slot 4870 |
| The table walk order is `0xff2f80 -> 0xff3000 -> 0xff3240 -> 0xff3260 -> 0xff33c0 -> 0xff33f0 -> 0xff3660` | **M** | growth22 iter01-04 and growth23 iter01-03 first-fault logs |
| 4,457 non-NULL C++ entries before slot 4904 returned | **M** | fault at slot 4904 (growth23/iter03) |
| `0xff2ee0` is an `atexit` thunk | **S** | body `push dtor; call 0xf520bc; pop; ret` |
| `0xff2ef0` is a static-init guard | **S** | guard byte + two `atexit` calls |
| The next expected fault is `0xff3720` | **S** | iter03 roots cover 4904/4905/4906/4907; next non-NULL is 4910 |
| 6,454 non-NULL entries remain after `0xff3660` (6,472 after `0xff2ee0`) | **S** | table + slice arithmetic |
| 659 distinct missing callees; first is `0x005ead40` at index 5208 | **S** | direct-call scan + slice diff |
| TLS callbacks executed at startup = 0 | **M/S** | TLS directory array + Windows first-NULL rule; our loader matches |
| The two TLS pointers also appear in the exit-time `_initterm` range | **M (data) / I (intent)** | `0x1076ac8..0x1076ad4` values |
| `Direct3DCreate9` is called at `0xcd5622` and `0xcd1fef` | **S** | string xrefs + `call eax` after `push 0x20` |
| The first factory COM calls are slots 4-8 (`GetAdapterCount` ... `GetAdapterDisplayMode`) at `0xce5e41`/`0xce5a76`/`0xce5ab6`/`0xce5ae4`/`0xce5cc9` | **S** | vtable offsets 0x10/0x14/0x18/0x1c/0x20 on the raw factory |
| `CreateDevice` is slot 16 at `0xcd8b72` | **S** | vtable offset 0x40 on the second factory |
| D3D9 device creation succeeds on host | **not claimed** | renderer gate not reached |
| No D3D9/PICA frame exists | **M** | all docs/logs; no frame artifact |

No claim above is based on a renamed symbol. Names such as `_initterm`,
`atexit`, `_heap_init` are used because the code shape and the CRT ABI are
unambiguous; the XI entry roles are marked inferred.

