# Papyrus PEX format and runtime load-path audit

Scope: read-only analysis of retail `E:\skyr\TESV.exe` and real compiled
scripts. Worktree branch `papyrus-pex`, base/HEAD
`31d58e600553c2468d97b35513b94c7c1e8eb869`. No game bytes are copied into this
repository. `d3d9*`, `win32_shims.c`, `guest.c`, `ir_*`, `staged/*`, `DevCaps`,
and `F:` were not modified.

## 1. Result in one paragraph

`tools/pex_dump.py` now parses the big-endian Skyrim PEX 3.2 container, including
strings, debug line tables, objects, properties, states/functions, typed
operands, variable arguments, and opcode histograms. Five synthetic tests pass,
and eight real `.pex` files (436 bytes through 996,899 bytes) parse with no
warnings or trailing bytes. Static disassembly identifies the full loader at
`0xC45410`, its header reader at `0xC42F20`, and the first file read at
`0xC42F46`. A real loader invocation was **not** claimed: the IR container has
all required code, but the harness lacks the game TEB/TLS allocator, initialized
C++ runtime objects, and the `VirtualMachine` registry that consumes the parsed
`ObjectTypeInfo`. Those requirements are listed precisely in §5.

## 2. Inputs and reproduction

* TESV.exe SHA-256:
  `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`
  (retail 1.9.32.0, PE32, image base `0x400000`, 17,462,064 bytes).
* The stated vanilla directory
  `E:\SteamLibrary\steamapps\common\Skyrim\Data\Scripts` does not exist on this
  host. Real compiled scripts were read from installed mods under
  `E:\SkyrimMods\authoria\mods` instead. They are ordinary Skyrim PEX 3.2 files,
  not generated fixtures.
* Format/opcode reference used for cross-checking:
  [Champollion v1.3.2 `Pex/FileReader.cpp`](https://github.com/Orvid/Champollion/blob/v1.3.2/Pex/FileReader.cpp)
  and
  [`Pex/Instruction.cpp`](https://github.com/Orvid/Champollion/blob/v1.3.2/Pex/Instruction.cpp).
  Every field width was then validated against the real files below.

```powershell
python tests/pex_dump_test.py
python tools/pex_dump.py Script.pex --strings --functions --instructions
python tools/pex_dump.py a.pex b.pex --json
```

The eight-file real batch used eight independent Python processes. Wall time
was **1.287 s** on this host; the synthetic suite took **0.213 s**.

## 3. Skyrim PEX 3.2 layout

All multibyte integers in the file are big-endian. Length-prefixed strings and
string-table indexes are `u16`; there is no NUL terminator in the stored string.
The parser rejects non-Skyrim magic/game IDs and unknown value/opcode types
instead of guessing operand widths.

| Section | Fields |
| --- | --- |
| Header | magic `u32` (`FA 57 C0 DE` on disk, constant `0xDEC057FA`), major `u8`, minor `u8`, game id `u16` (`1`), compilation time `u64`, source/user/computer strings |
| String table | count `u16`, then that many length-prefixed strings |
| Debug info | present `u8`; if present: source modification time `u64`, function count `u16`; each record has object/state/function string indexes, type `u8`, line count `u16`, and one source line `u16` per mapped instruction |
| User flags | count `u16`; each entry is a name string index and flag index `u8` |
| Objects | count `u16`; each object has name index, size `u32`, parent/doc/auto-state indexes, user flags `u32`, then variables, properties, and states |
| Variable | name/type indexes, user flags `u32`, default typed value |
| Property | name/type/doc indexes, user flags `u32`, flags `u8`; flags: read `0x01`, write `0x02`, auto-variable `0x04`; auto-variable index or getter/setter function bodies |
| State | name index, function count `u16`, function entries |
| Function | name/return/doc indexes, user flags `u32`, flags `u8` (`global=0x01`, `native=0x02`), parameter and local typed-name tables, instruction count `u16`, instructions |
| Instruction | opcode `u8`, fixed typed operands; `callmethod`, `callparent`, and `callstatic` additionally store an integer typed operand giving the vararg count, then that many typed operands |

Typed values are:

| Type byte | Payload |
| --- | --- |
| `0` | None |
| `1` | identifier, string index `u16` |
| `2` | string, string index `u16` |
| `3` | signed integer `i32` |
| `4` | IEEE-754 `f32` |
| `5` | Boolean `u8` |

Important details confirmed by the real files and the disassembler:

* Instruction count is a count of instructions, **not** a byte length.
* Branch operands are instruction-relative values handled by the VM, not native
  addresses.
* Skyrim 3.2 uses opcodes `0x00..0x24`. The tool also names later known opcodes
  so a newer file receives a specific warning rather than a width guess.
* Getter/setter debug records use the property name; the dumper matches those
  records to its synthetic `get_`/`set_` function names.

### Real-file results

| File | Bytes | Strings | Debug fns | Objects | Functions | Instructions | Code bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `Debris.pex` | 436 | 17 | 2 | 1 | 2 | 4 | 41 |
| `BWBKaiTEMove.pex` | 442 | 17 | 2 | 1 | 2 | 4 | 41 |
| `SphereWard.pex` | 1,119 | 50 | 5 | 1 | 5 | 13 | 148 |
| `nim__TIF__000BA09B.pex` | 695 | 29 | 3 | 1 | 3 | 7 | 75 |
| `_frost_compatibility.pex` | 207,346 | 1,832 | 76 | 1 | 76 | 5,798 | 142,130 |
| `SmoothCamMCM.pex` | 354,679 | 1,297 | 34 | 1 | 34 | 20,879 | 256,364 |
| `PDV_MCM.pex` | 139,902 | 2,175 | 173 | 1 | 173 | 5,548 | 60,466 |
| `PDV__ManagerQuest.pex` | 996,899 | 14,702 | 1,595 | 1 | 1,595 | 48,001 | 505,454 |
| **Total** | **1,701,518** | **20,119** | **1,890** | **8** | **1,890** | **80,254** | **964,719** |

All eight had zero parser warnings and zero trailing bytes. Aggregate opcode
counts were:

```text
01 iadd=419          02 fadd=23           03 isub=90
04 fsub=92           05 imul=15           06 fmul=93
07 idiv=5            08 fdiv=16           09 imod=8
0a not=1789          0c fneg=2            0d assign=5467
0e cast=8438         0f cmp_eq=6336       10 cmp_lt=321
11 cmp_lte=174       12 cmp_gt=283        13 cmp_gte=359
14 jmp=8161          15 jmpt=795          16 jmpf=9063
17 callmethod=14679  18 callparent=2       19 callstatic=4543
1a return=6549       1b strcat=3752       1c propget=5179
1d propset=77        1e array_create=40   1f array_length=34
20 array_getelement=2374                 21 array_setelement=1076
```

Full per-file SHA-256 values are emitted by `tools/pex_dump.py --json`; the
largest file hashes to
`bc24e372739c0da39cd7c761ba0a23b7688e93c6263f8ad21068d5ac682eb960`.

## 4. EXE load path and VM read point

### 4.1 Loader identity

The string `Magic number doesn't match, file is not a compiled script.` at
`0x1149BE4` has one code xref, `push` at `0xC4309A`, inside `0xC42F20`. Nearby
strings name every PEX header/debug failure. The full loader is `0xC45410`.

The `BSScript::ILoader` vtable is at `0x1149B98`:

| Slot | Address | Role |
| --- | --- | --- |
| `+0x00` | `0xC45DF0` | destructor |
| `+0x04` | `0xC42D70` | clone/factory |
| `+0x08` | `0xC42E80` | replace reader/writer |
| `+0x0C` | `0xC45410` | load compiled script |

`0xC42D70` allocates a `0x20`-byte loader and establishes this layout:

| Offset | Meaning |
| --- | --- |
| `+0x00` | vtable |
| `+0x04` | `BSScript::ErrorLogger*` |
| `+0x08` | `BSScript::IHandleReaderWriter*` |
| `+0x0C` | string-table object |
| `+0x1C` | parsed major version |
| `+0x1D` | parsed minor version |
| `+0x1E` | loader flags |

There is **no VM pointer or ObjectTypeInfo cache pointer in the loader**. It
parses and returns runtime type information; registration into the VM belongs
to the caller.

### 4.2 Load sequence

`0xC45410` performs:

```text
open class through reader vtable +0x18
  -> 0xC42F20  header (magic/version/game id/time/source strings)
  -> 0xC3FEF0  string table
  -> 0xC431C0  debug functions and line tables
  -> 0xC43AE0  user-flag definitions
  -> 0xC45220  object table
       -> 0xC40230  object name / ObjectTypeInfo allocation
       -> 0xC44560  one object: variables, properties, states, functions
  -> close reader through vtable +0x1C
```

The reader/writer ABI used by the loader is:

| Vtable slot | Use |
| --- | --- |
| `+0x10` | `Read(dst, size)`; returns zero on success |
| `+0x18` | open/check class name; Boolean result |
| `+0x1C` | close |
| `+0x20` | error/detail accessor used by the error logger |

The first actual VM file read is `call edx` at **`0xC42F46`**, where `edx` is
`reader->vtable[+0x10]`, the destination is a stack dword, and the requested
size is **4** (the magic). The code reads raw bytes, applies the reader's
endian-swap flag, and accepts `0xFA57C0DE`/`0xDEC057FA`. It then reads major and
minor version bytes and validates `3.0..3.2` plus game id `1`.

The concrete RTTI family includes
`BSScript::IHandleReaderWriter`, `SkyrimScript::BaseHandleReaderWriter`, and
`SkyrimScript::DataFileHandleReaderWriter`. The latter ultimately sits on the game's BSA/Win32
file layer. The previously audited external surface is `CreateFileA`,
`ReadFile`, `GetFileSizeEx`, `SetFilePointerEx`, `CloseHandle`, and
`FindFirstFileA`; a memory-backed implementation of the four reader vtable
methods is sufficient for a parser-only fixture, but not for VM registration.

### 4.3 IR availability

Both `E:\census_ir2\ir_container\tesv.irc` (90,900 functions) and
`E:\Skyrim_N3DS_build\ir_container\tesv.irc` (114,534 functions) contain valid
predecoded records for all key loader addresses:

```text
0xC45410  0xC42F20  0xC3FEF0  0xC431C0  0xC43AE0
0xC45220  0xC44560  0xC40230  0xC5F650
```

Therefore the blocker is not missing IR records.

## 5. Harness status and exact blocker

A success harness was deliberately **not** fabricated. Invoking `0xC45410` with
hand-made zeroed objects would only prove that the interpreter can call an
address; it would not prove PEX loading or ObjectTypeInfo/function registration.

The minimum real context is:

1. **TEB/TLS and game allocator.** `0xC45410` immediately reads `fs:[0x2c]` and
   global TLS-slot index `0x1BBEB54`. Object parsing calls `0xA48890`, which
   requires the thread-local memory-manager pointer at `TEB+0x4A0` and its
   allocation budget/state (`TEB+0x4B0` is also read by the shrink path). This is
   the first hard blocker before any object can be registered.
2. **Loader and reader objects.** A `0x20`-byte loader with the vtable above, a
   real or memory-backed `IHandleReaderWriter`, and a functioning
   `BSScript::ErrorLogger`. The reader must preserve stream position and return
   the exact error convention expected by slots `+0x10/+0x18/+0x1C/+0x20`.
3. **C++ runtime data and services.** Initialized string/vector/map/pool objects,
   allocator vtables, RTTI/type descriptors, TLS exception state, and the CRT /
   C++ exception paths used by cleanup. The IR container supplies code records,
   not these PE data objects or process initialization.
4. **ObjectTypeInfo and native-function factories.** `0xC44560` constructs the
   parsed type/function tables and inserts entries through map helpers such as
   `0xBC3A20`; those objects must be the real VM-compatible implementations, not
   host lookalikes.
5. **VirtualMachine registry caller.** The loader has no VM pointer. A separate
   VM caller must receive the parsed type, link parent/native functions, and
   insert it into the VM's class cache. Only that layer can answer “object and
   functions registered”.

The audit's existing CRT gaps (`operator new/delete`, `_snprintf` family,
SEH/`__CxxFrameHandler`) remain relevant to a standalone process fixture, but
the **first concrete blocker is the uninitialized TEB/TLS game allocator**, not
the PEX decoder and not the IR container.

### Next step

Build a process-image fixture that loads TESV's data sections, import thunks,
TEB/TLS slot, and the game memory manager, then invoke `0xC45410` with a
memory-backed reader and a real ErrorLogger. Instrument `0xC44560` and the VM
caller at the loader vtable slot `+0x0C` to record ObjectTypeInfo insertion and
function counts. Until that fixture exists, the honest status is:

* PEX format parsing: **verified on synthetic and real files**.
* EXE loader/read point: **verified statically with disassembly/xrefs**.
* Real loader execution and VM registration: **blocked by missing VM process
  context; not claimed**.

## 6. Tool tests

`tests/pex_dump_test.py` builds a synthetic PEX 3.2 containing an object, an
auto-property, `OnInit`, debug lines, and `nop/assign/return`. It checks header,
string/debug/object/function counts, instruction offsets and typed operands,
JSON CLI output, bad magic, truncation, unknown opcode, non-integer vararg
count, trailing bytes, and out-of-range string indexes. All five tests pass.
