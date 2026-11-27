# Havok host runtime: gravity-integration diagnostic

Worktree: `C:\Users\vasya\Documents\Skyrim_N3DS_wt\havok-world` (`havok-world`,
base `cc041e1`). This is a host-only diagnostic. It does not start TESV, a
renderer, or a Win32 process, and it does not modify the IR container.

## Result

`tests/havok_world_smoke.c` executes retail Havok code through the predecoded IR
container and the matching PE's read-only `.text`/`.rdata`/`.data` sections:

1. constructs the retail static `hkpWorldCinfo` at `0x132b468`;
2. constructs an `hkpWorld`, a unit-radius `hkpSphereShape`, and a dynamic
   `hkpRigidBody` at `(0,10,0)`;
3. adds the body to the world;
4. constructs the retail context manager with `0xd92100` and initializes it with
   `0xd921b0`, producing a real `hkpJobThreadPool` and `hkpJobQueue`;
5. synchronously invokes retail `hkpIntegrateMotionJob`/motion integration at
   `0xdf4410` for four `1/60 s` steps with gravity `(0,-9.8,0)`;
6. reads the transform through `0xd2fdc0`.

Observed output:

```text
world cinfo ctor   rc=0 result=0132b468
world ctor         rc=0 result=02002000
sphere ctor        rc=0 result=02003100
rigid body ctor    rc=0 result=02003400
world add body     rc=0 result=02001000
before position: (0, 10, 0)
context ctor       rc=0 result=01bb6a50
context init       rc=0 result=80000000
thread pool=6101a740 job queue=6101a9b0
step 1: y=9.99729 vy=-0.162695 accel=-9.76172
step 2: y=9.99187 vy=-0.325391 accel=-9.76172
step 3: y=9.98373 vy=-0.488086 accel=-9.76172
step 4: y=9.97288 vy=-0.650781 accel=-9.76172
after position:  (0, 9.97288, 0), vy=-0.650781
Havok world diagnostic PASS (world/body/context/queue/integrate; 4 gravity steps)
```

The body falls by `0.02712` units in four steps. The first measured acceleration
is `-9.76172`, consistent with `-9.8` plus the configured low damping.

Inputs used:

- container: `E:\census_ir2\ir_container\tesv.irc`, SHA-256
  `5f2144412dc63d7e15ba198b76637cf98126e40b9c980361eb99e7afcab40b1c`;
- matching PE: `E:\skyr\TESV.exe`, SHA-256
  `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`;
- PE `.text` virtual bytes (`0xc6931a` bytes) SHA-256
  `16b1da4f1b28f96191f8904735d15c32766ff1e40e7881204c0f1b1b139411f1`.

The container has 90,903 indexed functions and 90,110 decoded records. It does
not contain `.rdata`/`.data`; code also reads absolute jump tables from `.text`.
The harness consequently maps the matching PE's `.text`, `.rdata`, and `.data`
read-only into guest memory. A mismatched PE is not supported.

## Havok entries and fields used

All addresses are TESV VAs. `thiscall` means `this` in `ECX`; stack arguments
are listed in left-to-right source order.

| Purpose | Address | Signature / convention |
|---|---:|---|
| `hkpWorldCinfo` ctor | `0x00d38a70` | `hkpWorldCinfo* __thiscall(this)` |
| `hkpWorld` ctor | `0x00d54ca0` | `hkpWorld* __thiscall(this, defaults, info)` (`ret 8`) |
| `hkpSphereShape` ctor | `0x00d5f540` | `hkpSphereShape* __thiscall(this, float radius)` (`ret 4`) |
| `hkpRigidBody` ctor | `0x00d309d0` | `hkpRigidBody* __thiscall(this, const hkpRigidBodyCinfo&)` (`ret 4`) |
| Batch add | `0x00d51a60` | `void __thiscall(this, bodies, count, flag)` (`ret 0xc`) |
| Context-manager ctor | `0x00d92100` | `hkpContextManager* __thiscall(this)` |
| Context-manager init | `0x00d921b0` | `void __thiscall(this, config, value)` (`ret 8`) |
| Body transform getter | `0x00d2fdc0` | `hkTransform* __thiscall(this)`; translation `+0x30` |
| Integrate motions | `0x00df4410` | six stack args, synchronous one-body payload |
| Dispatcher (found, not driven end-to-end) | `0x00e8d8f0` | type `1` solver, type `11` integrate, type `12` callbacks |

Context-manager fields:

- `+0x10`: world pointer;
- `+0x18`: `hkpJobQueue*`;
- `+0x1c`: `hkpJobThreadPool*` (not the physics payload);
- singleton `0x1ba33f8` points back at the manager.

Motion/integration fields synthesized for the one-body payload:

- embedded `hkpSphereMotion` starts at `body+0xe0`;
- inline step info at `body+0x1e0`: `dt`, `dt`, `1/dt`;
- gravity/acceleration vector at `body+0x210`;
- inverse inertia/inverse mass at `body+0x1a0`;
- motion timestep at `body+0x1fe` (`0x3c88`, upper half of float `1/60`);
- position time scale at `body+0x198` (`0x3f80`, float `1.0`);
- low-damping solver class at `body+0x19a`;
- current position `body+0x140`, linear velocity `body+0x1b0`.

`hkpRigidBodyCinfo` fields used by the harness include shape `+0x04`, position
`+0x10`, rotation `+0x20`, linear velocity `+0x90`, damping `+0xb0/+0xb4`, mass
`+0xb8`, motion type `+0xbc`, collision response `+0xbf`, and broadphase-handle
creation `+0xc0`.

## Synthetic host state and stubs

- TLS indices `0x1bb6b08`, `0x1bb8674`, `0x1bbeb54` are `0`; IAT `0x106b130`
  returns the synthetic memory router. `TEB:[0x2c]` points at the TLS slot array.
- The router's `+0x28/+0x2c` fields point at a synthetic allocator with allocate,
  free, allocate-with-size-pointer, and realloc vtable entries.
- Allocation is a zeroed 16-byte-aligned 64 MiB bump arena at `0x61000000`.
- Critical-section entry/leave/delete and semaphore operations are host stubs.
  `0xe13d80` returns zero, so retail worker threads are disabled and integration
  is synchronous.
- Broadphase factories: `0x1bba480 = 0xdc2c00`, `0x1bba484 = 0xe91230`,
  `0x1bba488 = 0xe8ec80`.
- `0x1bb8360` points at a synthetic no-op error receiver; `0x1bba41c` points at
  a no-op optional world-step listener.
- `0xde8500` remains a scalar host equivalent for the container's MMX gap.
- `0xdc97a0` remains a host null-compaction helper because it has no exact
  container index entry.

Host AOT stubs must emulate guest stack cleanup. A `thiscall` function with one
stack argument advances guest `ESP` by 8 bytes; a cdecl `ret 0` stub advances it
by 4. Incorrect cleanup produces plausible but invalid faults deep in Havok.

## Exact remaining gaps

1. **Full queue dispatcher is not driven end-to-end.** The real context manager,
   thread pool, and job queue are constructed, but worker threads and queue lock
   processing remain host-stubbed. The test invokes the retail integrate payload
   synchronously. Dispatcher `0xe8d8f0` and job types `1/11/12` were identified,
   but a general multi-job solver/callback pipeline is still future work.
2. **No collision/solver contacts.** `0xde0340` is a no-op for this one-body
   smoke. Free fall is covered; contact response, islands, and constraints are
   not.
3. **Reachable MMX record is absent.** Container entry `0xde8500` is invalid:
   `IR: unsupported opcode mmx/sse at 00de858a` (`0f 6e`, `movq mm0,m64`). The
   scalar override remains until MMX decode/lift support or an AOT equivalent is
   merged.
4. **Unindexed internal vtable entry.** `0xdc97a0` exists in `.text` but has no
   exact container index entry; the harness supplies its null-compaction logic.
5. **CRT diagnostics are bypassed.** `0xd90510/0xd90560`, `0xf588af`, and
   `0xf5d79f` remain host stubs.
6. **Temporary allocator is synthetic.** `0xd1fbe0/0xd1fe10` use the bump arena;
   retail temporary allocator objects are not initialized.

## Reproduce

With generated `staged/` files present (the source-only worktree does not carry
them):

```powershell
powershell tools/run_host_tests.ps1 -Only havok_world -SkipPython `
  -TesvIrc E:\census_ir2\ir_container\tesv.irc `
  -TesvExe E:\skyr\TESV.exe
```

For this source-only worktree the binary can also be built manually against the
existing generated headers in `C:\Users\vasya\Documents\Skyrim_N3DS\staged`;
`staged/*` is not modified.
