# Havok host runtime: world/body/step diagnostic

Worktree: `C:\Users\vasya\Documents\Skyrim_N3DS_wt\havok-world` (`havok-world`,
base `31d58e6`). This is a host-only diagnostic. It does not start TESV, a
renderer, or a Win32 process, and it does not modify the IR container.

## Result

`tests/havok_world_smoke.c` successfully executes retail Havok code through the
predecoded IR container:

0. directly executes the retail MMX AABB clipping helper `0xde8500` on two
   synthetic entries and checks all 16 result lanes;
1. constructs the retail static `hkpWorldCinfo` at `0x132b468`;
2. constructs an `hkpWorld`;
3. constructs a unit-radius `hkpSphereShape`;
4. constructs a dynamic `hkpRigidBody` at `(0,10,0)`;
5. adds the body to the world;
6. runs the internal world-step entry `0xd52fe0` with `dt = 1/60`;
7. reads the body transform through `0xd2fdc0`.

Observed output:

```text
mmx aabb clip      rc=0 result=02003c20 ip=00000000 last=00de8500 allocs=0
mmx aabb clip: retail 0xde8500 PASS (2 entries / 16 lanes)
world cinfo ctor   rc=0 result=0132b468
world ctor         rc=0 result=02002000
sphere ctor        rc=0 result=02003100
rigid body ctor    rc=0 result=02003400
world add body     rc=0 result=02001000
before position: (0, 10, 0)
world step internal rc=0 result=80000000
after position:  (0, 10, 0)
Havok world diagnostic PASS (world/body/add/step; no solver/job queue)
```

**Gravity integration is not working yet.** The internal step returns normally
but does not move the body. The public multithreaded/sequential step path needs
an `hkpJobQueue` plus the integrate/solver/callback jobs; that pipeline is not
present in this harness. The test therefore asserts the exact current result
(translation unchanged) instead of claiming a physics pass.

Inputs used:

- container: `E:\Skyrim_N3DS_build\ir_container_mmx\tesv.irc`, 65,688,624 bytes,
  SHA-256 `4dd060a502d59716142ecbd9ccb1fc4ef7c0ede67d6a023972a3a3cf61cf0c12`;
- matching PE: `E:\skyr\TESV.exe`, SHA-256
  `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`;
- final host diagnostic binary SHA-256
  `bd05522184b05098216bfd229e7aa1f0452c96cfaff679ad446ba107bfc7567b`;
- PE `.text` virtual bytes (`0xc6931a` bytes) SHA-256
  `16b1da4f1b28f96191f8904735d15c32766ff1e40e7881204c0f1b1b139411f1`.

The container has 90,903 indexed functions and 90,110 decoded records. It does
not contain `.rdata`/`.data`; code also reads absolute jump tables from `.text`.
The harness consequently maps the matching PE's `.text`, `.rdata`, and `.data`
read-only into guest memory. A mismatched PE is not supported.

## Minimum Havok entries found

All addresses are TESV VAs. `thiscall` means `this` in `ECX`; stack arguments
are listed in left-to-right source order.

| Purpose | Address | Signature / convention | Container |
|---|---:|---|---|
| `hkpWorldCinfo` ctor | `0x00d38a70` | `hkpWorldCinfo* __thiscall(this)` | decoded |
| `hkpWorld` ctor | `0x00d54ca0` | `hkpWorld* __thiscall(this, const hkpWorldCinfo& defaults, const hkpWorldCinfo& info)` (`ret 8`) | decoded |
| `hkpSphereShape` ctor | `0x00d5f540` | `hkpSphereShape* __thiscall(this, float radius)` (`ret 4`) | decoded |
| `hkpRigidBody` ctor | `0x00d309d0` | `hkpRigidBody* __thiscall(this, const hkpRigidBodyCinfo& info)` (`ret 4`) | decoded |
| Batch add used by harness | `0x00d51a60` | `void __thiscall(this, hkpEntity* const* bodies, int count, hkBool)` (`ret 0xc`) | decoded |
| Internal step used by harness | `0x00d52fe0` | `void __thiscall(this, hkpPhysicsContext*, hkVector4* dt, hkBool final)` (`ret 0xc`) | decoded |
| Body transform getter | `0x00d2fdc0` | `hkTransform* __thiscall(this)`; translation is `+0x30` | decoded |

Relevant RTTI vtables:

- `hkpWorldCinfo` `0x01087c54`;
- `hkpWorld` `0x0116c23c`;
- `hkpSphereShape` `0x0116c8ac`;
- `hkpRigidBody` `0x011637e4`.

`hkpRigidBodyCinfo` fields used by the harness:

- `+0x00`: collision-filter info (`0`);
- `+0x04`: shape pointer (the sphere);
- `+0x10`: position `(x,y,z,w)`;
- `+0x20`: rotation quaternion;
- `+0x90`: linear velocity;
- `+0xb0/+0xb4`: angular/linear damping;
- `+0xb8`: mass;
- `+0xbc`: motion type (`2` = dynamic);
- `+0xc0`: broadphase-handle creation flag.

The retail static Cinfo is initialized at `0x132b468`, then `+0x54` is cleared
(no optional object/list), `+0xb4 = 1/60`, and `+0xbc = 4` before constructing the
world. Both ctor Cinfo arguments point at this static object.

## Synthetic globals, TLS and allocators

The harness supplies the following minimum state:

- TLS indices `0x1bb6b08`, `0x1bb8674`, `0x1bbeb54` are `0`; IAT
  `0x106b130` (`TlsGetValue`) points at a host stub returning the synthetic
  memory router. `TlsSetValue`, critical-section entry/leave/delete, and
  `GetModuleHandleW` are also stubbed.
- Synthetic router: `+0x28` and `+0x2c` point at the synthetic allocator.
- Allocator vtable: `+0x04` allocate, `+0x08` free, `+0x0c` allocate-with-size-
  pointer, `+0x10` free, `+0x14` realloc. Allocation is a zeroed 16-byte-aligned
  bump arena at `0x61000000` (16 MiB).
- Reference-count lock globals `0x1bb0ef0/0x1bb0ef4` point at a zeroed synthetic
  lock state/TLS index, disabling the game's threaded refcount lock.
- Broadphase factories: `0x1bba480 = 0xdc2c00`, `0x1bba484 = 0xe91230`,
  `0x1bba488 = 0xe8ec80`.
- `0x1bb8360` points at a synthetic no-op Havok error receiver.
- `0x1bb5e24 = 1.0f`; `0x1bbdda0`, `0x12d19a0`, and `0x1bb835c` are zero.
- Broadphase query pointers `0x1bbbc94/0x1bbbc98` are no-op stubs.

Host AOT stubs must emulate guest stack cleanup. For example, a `thiscall`
function with one stack argument advances guest `ESP` by 8 bytes (return
address + argument); a cdecl `ret 0` stub advances it by 4. Getting this wrong
produces convincing but invalid pointer faults deep inside Havok.

## Remaining gaps and resolved MMX item

1. **No solver/job queue.** `0xd52fe0` is only the internal step machinery. The
   integrate/solver/callback pipeline is dispatched through `0xe8d8f0`; relevant
   jobs include integrate motions `0xef7800`, solver `0xef9240`, and callback
   processing `0xef6b80`. A minimal completion needs a real or synthetic
   `hkpJobQueue` and a `hkpPhysicsContext`, not the current no-op context method.
2. **MMX record resolved (2026-11-27).** The rebuilt container decodes
   `0xde8500` as four blocks / 101 IR instructions (index status `ok`), including
   `0f 6f` at `0xde858a`. `0xde8610`, `0xde86e0`, `0xf7e184`, and the three CRT
   owners of the outside-Havok `psrlq` site are decoded too. The full-image MMX
   audit reports zero invalid MMX-owning functions. The scalar `0xde8500`
   override has been removed; `havok_world` passes against this container with
   the retail MMX helper in the execution path.
3. **Unindexed internal vtable entry.** `0xdc97a0` exists in `.text` but has no
   exact container index entry. The harness supplies its null-compaction logic.
   Container discovery must include internal vtable targets or this needs a
   permanent AOT entry.
4. **Collision dispatch is stubbed.** `0xde0340` is a no-op in this one-body
   smoke. That is sufficient for the current no-contact diagnostic, but not for
   a general world.
5. **CRT diagnostics are bypassed.** `0xd90510/0xd90560` (Havok version text),
   `0xf588af` (module wait), and `0xf5d79f` (thread-locale init) are host stubs.
   Full process initialization would provide these naturally.
6. **Temporary allocator is synthetic.** `0xd1fbe0/0xd1fe10` use the bump arena;
   the retail temporary allocator objects at `0x12d1c00/0x12d1bf8` are not
   initialized.

## Reproduce

With generated `staged/` files present (the source-only worktree does not carry
them):

```powershell
powershell tools/run_host_tests.ps1 -Only havok_world -SkipPython `
  -TesvIrc E:\Skyrim_N3DS_build\ir_container_mmx\tesv.irc `
  -TesvExe E:\skyr\TESV.exe
```

The test is registered in `tools/run_host_tests.ps1` when both inputs exist. It
is a diagnostic PASS for the current no-solver behavior, not a gravity pass.
For this source-only worktree the final binary was also built manually against
the existing generated headers in `C:\Users\vasya\Documents\Skyrim_N3DS\staged`;
`staged/*` was not modified.
