# Indexed all-functions IR container

Status: **new, standalone, not wired into the live tree yet.** Growth22 was
running while this was designed and measured; no existing build input, runtime
file, decoder, executor, Makefile or host-test script was edited.

| New file | Purpose |
|---|---|
| `source/ir_container.h` | public API, on-disk constants, flags, status codes |
| `source/ir_container.c` | open/index/lookup, lazy record load, bounded LRU, predecoded executor |
| `tools/ir_pack.py` | builds a container from `TESV.exe` + `functions.json` with the real C decoder |
| `tests/ir_container_pack.c` | C packer invoked by `tools/ir_pack.py` |
| `tests/ir_container_equiv.c` | AOT vs slice-IR vs container differential test |
| `tests/ir_container_integrity.c` | open/lookup/LRU/corruption/short-read test |
| `tests/ir_container_test.py` | standalone build + run driver for both tests |

The live slice currently has IR extents for ~33,646 selected functions while
discovery finds **90,900** functions in TESV. A call to a discovered function
that is absent from the slice/AOT table raises `unknown function/import`. The
decoder does not lack the opcode; the function is simply not in the table. This
container indexes every discovered function, loads one decoded function record
from SD on demand, and executes that record without invoking the runtime
decoder.

AOT still wins. The container is normally a fallback tier *below* slice IR,
not a replacement: a measured hot/forced function stays AOT, and the default
dispatch rule is

```
AOT FnEntry -> slice IR table -> container record -> explicit unknown fault
```

The slice IR table comes before the container because it is the verified/hot
set that growth has explicitly lifted. An **invalid** container entry would
fault before the slice could serve an address that the slice already
implements, which stalled growth.

There is one explicit exception: an index entry carrying
`N3DS_IRC_FN_PREENTRY` has a decoded cold block before its callable entry.
Slice IR starts its frame at the exact entry and cannot execute that block, so
dispatch is `AOT -> pre-entry container record -> slice IR -> container` for
those entries only. The flag is set by the packer only after a real decoded
conditional branch proves an already discovered pre-entry leader reachable.

The slice IR table stays as a fallback so a build/container failure cannot
regress the current behaviour.

### 1.0 Block splitting after a call

`n3ds_ir_encode_block()` ends a block at a call because the interpreter pushes
the return address and resumes in a fresh block. Discovery keeps a call and the
code after it in one span, so the packer now re-encodes from `info.end_ip` while
the span continues and the terminator was `IR_OP_CALL`/`IR_OP_CALL_IND`. Without
this, executing a container function that calls and then continues faulted with
`block ... is not in the predecoded record` (first seen at `0x00402dcf`).
`IrBlockInfo.term_op` carries the terminator opcode. This makes the record
complete; it also turns 36 previously silent incomplete records into explicit
`not decoded` entries.

---

## 1. Container format (version 1)

All integers are little-endian. All offsets are bytes. The file is one
self-contained image; there is no sidecar index file and no per-run
recomputation.

```
offset 0                      IrcFileHeader            64 bytes
offset 64                     index[function_count]   40 bytes each, sorted by addr
align8                        function records        each 8-byte aligned
end                           (no trailer; total_bytes in the header)
```

### 1.1 File header (64 bytes)

| Offset | Type | Field |
|---:|---|---|
| 0 | `u8[8]` | magic `"IRCNT001"` |
| 8 | `u32` | version (`1`) |
| 12 | `u32` | header bytes (`64`) |
| 16 | `u32` | file flags (`HAS_AOT`, `HAS_FORCED`, `COMPLETE`, `LE`) |
| 20 | `u32` | function_count |
| 24 | `u32` | decoded_count |
| 28 | `u32` | index_offset (`64`) |
| 32 | `u32` | index_bytes (`function_count * 40`) |
| 36 | `u32` | index_entry_bytes (`40`) |
| 40 | `u32` | data_offset (8-aligned) |
| 44 | `u32` | data_bytes |
| 48 | `u32` | index CRC32 |
| 52 | `u32` | data CRC32 |
| 56 | `u32` | header CRC32 (computed with this field zero) |
| 60 | `u32` | total_bytes (`data_offset + data_bytes`) |

`N3DS_IRC_FILE_LE` is mandatory. `total_bytes` must equal the real file size;
a truncated file is rejected at open. `COMPLETE` means the index covers every
function the packer was given, not that every function decoded.

### 1.2 Function index entry (40 bytes)

| Offset | Type | Field |
|---:|---|---|
| 0 | `u32` | `addr` exact callable entry |
| 4 | `u32` | `end` exclusive extent (max discovery `hi`) |
| 8 | `u32` | flags (`DECODED`, `AOT`, `FORCED`, `COMPLETE`, `INVALID`, `PREENTRY`) |
| 12 | `u16` | decode status (`OK`, `UNSUPPORTED`, ...) |
| 14 | `u16` | block_count |
| 16 | `u32` | insn_count |
| 20 | `u32` | record_offset (absolute, 8-aligned) |
| 24 | `u32` | record_size (0 when not decoded) |
| 28 | `u32` | record CRC32 |
| 32 | `u32[2]` | reserved, zero |

The index is checked at open for: strict address ordering, unique addresses,
`end > addr`, status range, decoded/record consistency, record offsets inside
the data section, alignment, and CRC. A bad index fails `n3ds_irc_open()` with
`N3DS_IRC_ERR_INDEX_CRC`/`N3DS_IRC_ERR_INDEX_BOUNDS`; it is never partially
trusted.

Every discovered function has an entry, including functions the decoder cannot
decode. An invalid entry has `INVALID`, `decode_status != OK`, and
`record_size == 0`. Executing it faults explicitly; it is never silently
skipped or retried as if it were absent. `PREENTRY` is valid only on decoded
entries whose first sorted block starts below `addr`; it selects the
container-aware frame instead of the exact-entry slice-IR frame.

### 1.3 Function record

```
IrcRecordHeader (40 bytes)
IrcBlockEntry[block_count] (20 bytes each)
align4
u32 stream words, 4-byte aligned, contiguous
align8 padding
```

Header:

| Offset | Type | Field |
|---:|---|---|
| 0 | `u32` | magic `"IRCF"` (`0x46435249`) |
| 4 | `u16` | record version (`1`) |
| 6 | `u16` | record flags |
| 8 | `u32` | entry (matches index) |
| 12 | `u32` | end (matches index) |
| 16 | `u32` | block_count (matches index) |
| 20 | `u32` | insn_count (matches index) |
| 24 | `u32` | blocks_offset (`40`) |
| 28 | `u32` | block_bytes (`block_count * 20`) |
| 32 | `u32` | stream_bytes |
| 36 | `u32` | reserved |

Block entry:

| Offset | Type | Field |
|---:|---|---|
| 0 | `u32` | `start` guest address |
| 4 | `u32` | `stream_off` relative to record start, 4-aligned |
| 8 | `u32` | `stream_words` |
| 12 | `u16` | `x86_len` |
| 14 | `u8` | `insn_count` |
| 15 | `u8` | flags (`1` = terminator) |
| 16 | `u32` | reserved |

The stream is exactly the `ir_decode.c` word stream. `IrExecBlock` is
reconstructed from the block entry (`start`, `x86_len`, `insn_count`,
`stream_words`, `stream`).

### 1.4 Determinism and alignment

* Records start at 8-byte aligned file offsets; block streams at 4-byte aligned
  record-relative offsets. The format is valid for 32-bit ARM unaligned-load
  restrictions.
* The packer sorts functions by address and discovery ranges by start. Two runs
  over the same inputs produce byte-identical files.
* CRC32 is IEEE 802.3 (polynomial `0xEDB88320`, init/final `0xffffffff`).
The header and index CRCs are verified at open; the per-record CRC is verified
on every lazy load. The header's `data_crc32` covers the whole data section and
is available for an optional full-image check; it is not forced at open so
opening remains a single index read.
* No timestamps, paths, or host pointers are written into records.

---

## 2. Packing every discovered function

```
python tools/ir_pack.py \
    --exe E:/skyr/TESV.exe \
    --functions E:/Skyrim_N3DS_build/analysis/tesv/functions.json \
    --out E:/Skyrim_N3DS_build/ir_container \
    --profile E:/Skyrim_N3DS_build/host/alloc_probe_20261124-192723/tesv_profile.csv
```

The driver only prepares the `IRM1` input, compiles `tests/ir_container_pack.c`
with the real `source/ir_decode.c`, and adds profile analysis. Every block in a
record is produced by `n3ds_ir_encode_block()`; there is no Python
re-implementation of the IR encoding.

Important packing rule: discovery ranges are **basic-block leaders**, and two
leaders can overlap when one decoded block runs past another leader (a branch
target inside a later block). The interpreter decodes from whichever address
control flow reaches, so the packer emits a record block for *every* leader,
not just the first block covering an address. This is what makes a later
branch target a cache hit instead of a block miss. Only exact duplicate starts
are merged. Discovery ranges before the function's exact entry are ignored:
the interpreter's frame starts at the entry and cannot enter them from this
function.

### 2.1 Full TESV measurement

```
functions            90,900
decoded              90,063  (99.08%)
invalid                 837  (0.92%)
  unsupported opcode    812
  no terminator          25
blocks              722,343
x86 instructions  3,926,482   (per record; overlapping leader blocks counted per record)
```

The instruction count is the sum over record blocks, not a unique-code count: a
leader inside a later block is emitted separately so branch targets are cache
hits. The unique discovery figures remain the ones in `docs/IR.md`.

Invalid functions are listed individually in `tesv_invalid.csv`:
`addr,end,status,error` (for example `push es`, `push ss`, `gs segment`,
`mmx/sse`, `address-size prefix`, `aas`, `das`, and 25 shared cold-block
`block has no terminator` cases). `tesv_invalid_addrs.txt` is the same list as
hex addresses, ready to use as a forced-AOT input. The same entries are in the
index with `INVALID` and fault with the status in the message.

The 812 unsupported functions are exactly why "the container has all
functions that decode" must not mean "pretend the rest decoded". They stay
visible and fault explicitly; the existing forced-AOT selection can keep
those addresses AOT.

### 2.2 Size

```
container          48,869,656 bytes   (46.61 MiB)
  index             3,636,000 bytes   (90,900 * 40)
  data             45,233,592 bytes
records             90,063
record size        min 64, max 57,552, avg 502.2 B
                   p50 184, p90 1,032, p99 4,912
```

Record-size histogram (bytes / functions):

| Bucket | Functions |
|---|---:|
| 64–128 | 32,163 |
| 128–256 | 20,208 |
| 256–512 | 16,525 |
| 512–1,024 | 11,985 |
| 1,024–2,048 | 5,474 |
| 2,048–4,096 | 2,442 |
| 4,096–8,192 | 922 |
| 8,192–16,384 | 284 |
| 16,384–32,768 | 49 |
| 32,768–65,536 | 11 |

`tesv_index.csv` (in `_work/`) has the per-function row:
`addr,end,record_size,blocks,insns,status,flags`.

### 2.3 AOT / forced overlay

The packer accepts the existing selection files:

```
python tools/ir_pack.py ... --aot sel/aot.txt --forced v1_fail.txt
```

`aot.txt` entries get `N3DS_IRC_FN_AOT`; `v1_fail.txt` entries get
`N3DS_IRC_FN_FORCED`. The flags are reporting/selection metadata. Runtime
precedence is enforced by the AOT predicate (§3.4), so a stale container cannot
steal a call from AOT.

### 2.4 SD package

Only `tesv.irc` is needed at runtime. Copy it to the path the runtime opens:

```
sdmc:/Skyrim/ir/tesv.irc
```

`tesv_container.json` (hashes, counts, histogram, profile model) and
`tesv_invalid.csv` are diagnostics and are not required on the SD card.

---

## 3. Runtime API

```c
N3dsIrc *n3ds_irc_open_ex(const char *path, int *status);
N3dsIrc *n3ds_irc_open(const char *path);
void     n3ds_irc_close(N3dsIrc *irc);

int n3ds_irc_lookup(const N3dsIrc *irc, uint32_t addr, N3dsIrcEntry *out);
int n3ds_irc_entry_at(const N3dsIrc *irc, unsigned ordinal, N3dsIrcEntry *out);

int n3ds_irc_cache_init(N3dsIrc *irc, size_t max_bytes, unsigned max_records);
void n3ds_irc_cache_clear(N3dsIrc *irc);
int n3ds_irc_load(N3dsIrc *irc, uint32_t addr);
int n3ds_irc_pin(N3dsIrc *irc, uint32_t addr);
int n3ds_irc_unpin(N3dsIrc *irc, uint32_t addr);
int n3ds_irc_is_loaded(const N3dsIrc *irc, uint32_t addr);
void n3ds_irc_recover(N3dsIrc *irc);

void n3ds_irc_set_aot_predicate(N3dsIrc *, N3dsIrcAotPredicate, void *user);
int  n3ds_irc_exec(N3dsIrc *, CPU *c, uint32_t addr);
int  n3ds_irc_exec_forced(N3dsIrc *, CPU *c, uint32_t addr);
int  n3ds_irc_exec_status(N3dsIrc *, CPU *c, uint32_t addr, int allow_aot);

void n3ds_irc_stats(const N3dsIrc *, N3dsIrcStats *out);
void n3ds_irc_stats_reset(N3dsIrc *);
```

### 3.1 Open and lookup

`n3ds_irc_open_ex()` reads the 64-byte header and the whole index into RAM
(3.6 MiB for 90,900 functions), verifies the header CRC, index CRC, ordering,
bounds and alignment, then returns. It does **not** read function records.
Host measurement: about 20 ms for the full index (37 ms on the first cold run). On 3DS the same
work is one 3.6 MiB sequential SD read plus CRC; records stay lazy.

`n3ds_irc_lookup()` is an exact-entry binary search. It returns
`N3DS_IRC_ERR_NOT_FOUND` for an address outside the index and `N3DS_IRC_OK`
for invalid entries too (the caller can inspect `flags`/`decode_status`).

### 3.2 Lazy record load and CRC

`n3ds_irc_load()`/`n3ds_irc_pin()` read exactly one `record_size` span from
`record_offset`, then check:

1. `fseek`/`fread` result (short read -> `ERR_RECORD_SHORT`),
2. the index's record CRC32,
3. record magic/version, entry/end/count agreement,
4. descriptor ordering, bounds, stream alignment and stream bounds.

A failed load never installs a partial record and consumes no cache slot. The
same failure returns the same status on every retry; there is no hidden
"retry decode". If a valid record for that address is already resident, it is
returned before any I/O.

### 3.3 Bounded LRU, pin/unpin

`n3ds_irc_cache_init(irc, max_bytes, max_records)` allocates a fixed slot table
and enforces both limits. Eviction is least-recently-used among **unpinned**
records, with a monotonically increasing use tick and deterministic slot order
as the tie-break. A cache full of pinned records returns
`N3DS_IRC_ERR_CACHE_PINNED` rather than evicting a record in use.

`n3ds_irc_pin()` loads if needed and increments a pin; `n3ds_irc_unpin()`
releases one pin. `n3ds_irc_exec*()` pins the function record for the duration
of the call, so nested guest calls can load other records without invalidating
the executing one.

A guest fault `longjmp`s past `n3ds_irc_exec()`, so the integration layer must
call `n3ds_irc_recover(irc)` from the invocation's fault path (the exact hunk
is in §5). It drops executor pins; user pins are untouched. This is the only
non-local cleanup rule.

`N3dsIrcStats` reports opens, lookups, cache hits/misses, load attempts, loads,
loaded bytes, load nanoseconds, evictions/evicted bytes, pins/unpins, short
reads, CRC/record/I-O errors, invalid entries, block hits/misses, execs, AOT
preemptions, current resident bytes/records, and `redecodes` (always zero in
container execution).

### 3.4 AOT overlay precedence

`n3ds_irc_set_aot_predicate()` installs the same exact-entry test the runtime
already uses. `n3ds_irc_exec()` calls it **before** any lookup/load and returns
`N3DS_IRC_TIER_AOT` without touching the cache. `n3ds_irc_exec_forced()` skips
the predicate for differential tests. The integration uses the status form so
"not in container" can fall through to the slice IR table, but a known invalid
or unreadable container entry faults explicitly instead of silently decoding.

### 3.5 Predecoded execution, no silent re-decode

`source/ir_container.c` reuses the real interpreter body from `source/ir_exec.c`
as a unity include, renaming only its three public entry points. The container
drives the same `exec_block()` with `IrExecBlock`s built directly from the
loaded record:

* `irc_fetch_block()` binary-searches the record's block descriptors and
  returns pointers into the loaded record;
* `irc_run()` uses the record's exact entry and `end`;
* a requested block that is not in the record calls
  `rt_unimpl(..., "IR container: block ... is not in the predecoded record")`.

There is no call to `n3ds_ir_encode_block()`, `n3ds_ir_fetch_block()` or the
decode cache anywhere in the execution path. The differential test asserts both
`stats.redecodes == 0` and that the slice decoder's block counter does not move
during a container run.

Cost of this pass: the unity include duplicates the interpreter body, measured
at 46,733 bytes of ARM11 `.text` for `ir_container.o` (`-O2`, armv6k, with
3,080 bytes of `.bss` for executor frames). A later refactor can remove the
duplicate without changing the container format (see §5.7).

---

## 4. Differential and failure tests

Standalone, no game data and no host-test script needed:

```
.venv/Scripts/python.exe tests/ir_container_test.py
```

It builds `build_ir_container/`, packs the self-authored `tests/ir_corpus.h`
through the real C decoder, and runs:

* every corpus function with 4 deterministic inputs in AOT, slice-IR and
  container modes, comparing result, full `CPU`, and guest writes;
* `arith`/`logic`/`shifts`/`muldiv`/`byteops`/`memops`/`strings`/`flags`/
  `loopback`/`direct`/`indirect`/`x87ops`/`sseops`/`bench*`, plus the known
  `cmc` AOT divergence (container is compared with IR there);
* explicit `push es` unsupported entry -> `IR container: function ... is not
  decoded (unsupported opcode)`;
* AOT predicate precedence: one predicate call, `TIER_AOT`, no container load,
  no second tier dispatch; forced container execution still works;
* unknown address -> `ERR_NOT_FOUND`, then a fault from the dispatch caller;
* no re-decode (`redecodes == 0`, decoder block counter unchanged);
* open/lookup/entry iteration;
* LRU: 2 slots, both pinned -> `CACHE_PINNED`; unpin one -> eviction of exactly
  that record; re-pin loaded record -> cache hit, no extra load; clear;
* corruption: bad magic, bad version, bad header CRC, bad index CRC, bad record
  CRC (twice, deterministic), truncated file, missing file;
* short read after open: truncate a record beyond the stdio buffer, two pins
  both return `ERR_RECORD_SHORT`, `loads == 0`;
* AOT/forced packer flags.

Full real container verification:

```
.\build_ir_container\ir_container_integrity.exe verify-all ^
    E:\Skyrim_N3DS_build\ir_container\tesv.irc
verify-all: 90900 functions, 90063 decoded, 90063 loaded, 0 bad,
            loads 90063 bytes 45233592 ... evictions 89551 short 0 crc 0 record 0
```

Host timing for the full container: open ~20 ms, verify-all (every record read,
CRC-checked, 8 MiB/512-slot cache) ~290 ms.

---

## 5. Exact later integration (not applied in this pass)

### 5.1 `source/n3ds_lift_runtime.c` — include and state

```diff
 #include "n3ds_lift_runtime.h"
+#include "ir_container.h"
 #include "win32_shims.h"
@@
 static GuestMemory *memory;
 static const FnModule *modules;
 static unsigned module_count;
 static int ever_bound;
+static N3dsIrc *ir_container;
+static const FnEntry *lookup(uint32_t addr);
+void rt_unimpl(CPU *c, uint32_t addr, const char *what) __attribute__((noreturn));
 
+static int container_aot_pred(void *user, uint32_t addr) {
+    (void)user;
+    return lookup(addr) != NULL;
+}
+
+/* Status form so "not in container" can fall through to the slice IR table,
+ * while a known-invalid or unreadable record faults explicitly. */
+static int container_dispatch(CPU *c, uint32_t addr) {
+    if (!ir_container) return 1;
+    int rc = n3ds_irc_exec_status(ir_container, c, addr, 1);
+    if (rc == N3DS_IRC_TIER_CONTAINER) return 0;
+    if (rc == N3DS_IRC_TIER_AOT) {
+        const FnEntry *e = lookup(addr);
+        if (e) { e->fn(c); return 0; }
+        return 1; /* stale predicate: let the caller decide */
+    }
+    if (rc == N3DS_IRC_ERR_NOT_FOUND) return 1;
+    if (rc == N3DS_IRC_ERR_UNDECODED)
+        rt_unimpl(c, addr, "IR container: entry is not decoded");
+    rt_unimpl(c, addr, n3ds_irc_strstatus(rc)); /* no silent re-decode */
+}
```

### 5.2 `n3ds_lift_bind_modules()` — open + cache init + predicate

```diff
     memory=m;modules=list;module_count=n;ever_bound=1;
 #if defined(__ELF__) || defined(__3DS__) || defined(_WIN32)
     if (g_ir_table && g_ir_count && n3ds_ir_bind(g_ir_table,g_ir_count)) return -1;
 #endif
+    {
+        const char *path = getenv("N3DS_IRC_PATH");
+#ifdef __3DS__
+        if (!path) path = "sdmc:/Skyrim/ir/tesv.irc";
+#else
+        if (!path) path = "tesv.irc";
+#endif
+        int st = 0;
+        ir_container = n3ds_irc_open_ex(path, &st);
+        if (ir_container) {
+            if (n3ds_irc_cache_init(ir_container, 4u << 20, 64u) != N3DS_IRC_OK) {
+                n3ds_irc_close(ir_container);
+                ir_container = NULL;
+            } else {
+                n3ds_irc_set_aot_predicate(ir_container, container_aot_pred, NULL);
+                fprintf(stderr, "IR container: %u functions (%u decoded) from %s\n",
+                        n3ds_irc_function_count(ir_container),
+                        n3ds_irc_decoded_count(ir_container), path);
+            }
+        }
+        if (!ir_container)
+            fprintf(stderr, "IR container absent (%s: %s); using AOT + slice IR\n",
+                    path, n3ds_irc_strstatus(st));
+    }
     return 0;
 }
```

`4 MiB / 64 records` is the API default. The recorded profile working set is
1.68 MiB and 99 % of calls touch 0.053 MiB, but the measured 64-record model
tops out at 97.1 %; **8 MiB / 256 records** is the recommended device setting
(99.0 % modeled) if the memory budget allows it.

### 5.3 `n3ds_lift_has_function()` and dispatch

```diff
-int n3ds_lift_has_function(uint32_t address){return lookup(address)!=NULL || n3ds_ir_has(address);}
+int n3ds_lift_has_function(uint32_t address){
+    if (lookup(address)) return 1;
+    if (ir_container) {
+        N3dsIrcEntry e;
+        if (n3ds_irc_lookup(ir_container, address, &e) == N3DS_IRC_OK) return 1;
+    }
+    return n3ds_ir_has(address);
+}
@@
 int n3ds_rt_dispatch(CPU *c,uint32_t addr) {
     const FnEntry *e=lookup(addr);
     if(e){e->fn(c);return 0;}
+    if(!container_dispatch(c,addr))return 0;
     if(n3ds_ir_has(addr)){n3ds_ir_call(c,addr);return 0;}
     return -1;
 }
```

`container_dispatch()` returns 1 only for `ERR_NOT_FOUND`, so the slice IR table
is still consulted for addresses the container does not index. An indexed but
undecoded function faults; it is not handed to the decoder.

### 5.4 `rt_call_ic()`

```diff
 void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache) {
     const FnEntry *e=lookup(addr);
     if(e){*cache=e;e->fn(c);return;}
+    if(!container_dispatch(c,addr)){*cache=NULL;return;}
     if(n3ds_ir_has(addr)){
         *cache=NULL;n3ds_ir_call(c,addr);return;
     }
     rt_unimpl(c,addr,"unknown indirect function");
 }
```

The per-site cache still stores only AOT `FnEntry`s, exactly as today. A
container target re-enters `rt_call_ic`; no generated code changes and no
duplicate dispatch.

### 5.5 Fault recovery and release

```diff
-    if(interrupted){active=f.prev;c->r[4]=sp;if(interrupted==2){*result=exit_code;return N3DS_INVOKE_EXIT;}return N3DS_INVOKE_TRAP;}
```

becomes

```diff
     if(interrupted){
+        if(ir_container)n3ds_irc_recover(ir_container);
         active=f.prev;c->r[4]=sp;
         if(interrupted==2){*result=exit_code;return N3DS_INVOKE_EXIT;}
         return N3DS_INVOKE_TRAP;
     }
```

and

```diff
-void n3ds_lift_release(void){if(!active){memory=NULL;modules=NULL;module_count=0;}}
+void n3ds_lift_release(void){
+    if(!active){
+        if(ir_container){n3ds_irc_close(ir_container);ir_container=NULL;}
+        memory=NULL;modules=NULL;module_count=0;
+    }
+}
```

### 5.6 Build and package

`Makefile` targets that compile `n3ds_lift_runtime.o` need one more object:

```diff
-export OFILES := ir_bench_main.o ir_aot_fixture.o guest.o n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o fpu.o
+export OFILES := ir_bench_main.o ir_aot_fixture.o guest.o n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o ir_container.o fpu.o
@@
-export OFILES += n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o rt_prof.o fpu.o ...
+export OFILES += n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o ir_container.o rt_prof.o fpu.o ...
@@
-export OFILES := n3ds_boot.o tesv_slice_main.o ... n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o ...
+export OFILES := n3ds_boot.o tesv_slice_main.o ... n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o ir_container.o ...
```

`ir_container.c` needs the same include paths as `ir_exec.c` (`source`,
`staged`). No new libraries.

Package:

```
python tools/ir_pack.py \
  --exe E:/skyr/TESV.exe \
  --functions E:/Skyrim_N3DS_build/analysis/tesv/functions.json \
  --out E:/Skyrim_N3DS_build/ir_container \
  --aot <selection>/aot.txt --forced <selection>/forced.txt \
  --profile <run>/tesv_profile.csv
copy /y E:\Skyrim_N3DS_build\ir_container\tesv.irc <sdroot>\Skyrim\ir\tesv.irc
```

The host test can set `N3DS_IRC_PATH` to a generated corpus container and the
3DS build uses `sdmc:/Skyrim/ir/tesv.irc`.

### 5.7 Removing the unity-include duplicate (follow-up, optional)

This pass reuses `ir_exec.c` by including it in `ir_container.c`. That keeps
semantics identical and costs 46,313 bytes of ARM11 `.text`. To remove the
duplicate later, add a block-source entry point to `ir_exec.c`:

```c
/* source/ir_core.h */
typedef int (*N3dsIrBlockSource)(void *user, uint32_t start,
                                 uint32_t func_end, IrExecBlock *out);
void n3ds_ir_run_with(CPU *c, uint32_t addr, uint32_t func_end,
                      N3dsIrBlockSource source, void *user);
```

`n3ds_ir_run_with()` is the existing `n3ds_ir_run()` loop with
`n3ds_ir_fetch_block()` replaced by `source(user, ...)`. Then delete the
`#define/#include "ir_exec.c"` block from `ir_container.c` and call:

```c
int n3ds_irc_block_source(void *user, uint32_t start, uint32_t func_end,
                          IrExecBlock *out);
...
n3ds_ir_run_with(c, addr, e.end, n3ds_irc_block_source, irc);
```

The container format and all tests are unaffected.

---

## 6. Cache working-set for a recorded profile

Profile: `E:\Skyrim_N3DS_build\host\alloc_probe_20261124-192723\tesv_profile.csv`
(`addr,hits`, 12,511 rows). The profile is **aggregate**, not a temporal trace,
so the tool reports two separate things.

### 6.1 Static hot-set coverage (exact, from recorded hits)

| Calls covered | Functions | Record bytes |
|---:|---:|---:|
| 50 % | 1 | ~0.00 MiB |
| 90 % | 14 | 0.005 MiB |
| 99 % | 106 | 0.053 MiB |
| 100 % | 12,509 | 1.677 MiB |

This is the number that matters for the cache: the whole valid profile working
set is **1.68 MiB**, and 99 % of calls need **53 KiB**.

### 6.2 Synthetic temporal LRU (clearly modeled)

Rows present in the container: 12,511 / 12,511; 12,509 are valid records, two
(`0xfcee00`, `0x102cc90`) are indexed but invalid `block has no terminator`
entries. A deterministic frequency-weighted trace of 1,000,000 accesses
(xorshift seed `0x9E3779B9`) gives:

| Cache | Record cap | Hit rate | Loads | Peak records |
|---|---:|---:|---:|---:|
| 1 MiB | 64 | 97.12 % | 28,826 | 64 |
| 4 MiB | 64 | 97.12 % | 28,826 | 64 |
| 4 MiB | 256 | 98.98 % | 10,159 | 256 |
| 4 MiB | unlimited | 99.38 % | 6,184 | 6,184 |

The byte cap is not the binding constraint at 1–16 MiB because the hot records
are small; the **record cap** is. With the runtime default of 64 records the
modeled hit rate is 97.1 %; raising the cap to 256 raises it to 99.0 %. The
modeled 6,184 distinct sampled records are the frequency-weighted active set.
For a temporal claim, replace the model with a recorded access trace; the
static coverage table is trace-independent.

Default runtime cache: 4 MiB / 64 records. The measured 99 % static working set
is only 106 records / 0.053 MiB, so 64 records is borderline for the long tail;
**8 MiB / 256 records** is the recommended device setting if the memory budget
allows it. The byte cap can stay at 4–8 MiB.

---

## 7. Failure semantics summary

| Situation | Result |
|---|---|
| container file missing | `open` fails; caller keeps AOT + slice IR |
| header magic/version/CRC bad | `ERR_HEADER` / `ERR_VERSION`; no handle |
| index CRC/order/bounds bad | `ERR_INDEX_CRC` / `ERR_INDEX_BOUNDS`; no handle |
| file shorter than `total_bytes` | `ERR_HEADER` at open |
| record short read | `ERR_RECORD_SHORT`; slot not installed; repeatable |
| record CRC mismatch | `ERR_RECORD_CRC`; slot not installed; repeatable |
| record header/descriptor malformed | `ERR_RECORD_BAD`; slot not installed |
| entry present but not decoded | `ERR_UNDECODED`; exec faults with status name |
| entry absent | `ERR_NOT_FOUND`; dispatch falls through to slice IR |
| record larger than cache budget | `ERR_TOO_LARGE`; no eviction storm |
| all cache slots pinned | `ERR_CACHE_PINNED`; no unpinned victim churn |
| AOT predicate matches | `TIER_AOT`; no lookup/load/dispatch |
| block missing from record | explicit fault naming the block |
| guest fault during exec | caller must call `n3ds_irc_recover()` |

No failure path decodes guest bytes to hide a container problem. `redecodes`
stays zero.
