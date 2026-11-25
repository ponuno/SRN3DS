# Texture residency runtime integration

Status: **core/COM ownership wiring applied**. PICA/TEV draw and shader behavior
is preserved; the texture modules now ship in graphics, COM, and TESV builds.

## Implemented behavior

| Path | Guest source | GPU source | Binding |
|---|---|---|---|
| `D3DPOOL_DEFAULT` | retained for CPU lock/readback | allocated at creation, marked ready after upload | allowed after level 0 upload |
| `D3DPOOL_MANAGED` | retained as the restoration source | same lifecycle as DEFAULT | allowed after level 0 upload |
| `D3DPOOL_SYSTEMMEM` | retained, cache-owned | none | rejected |
| direct `d9_create_texture_sktx()` | none; SKTX is parsed and released by the caller | ETC1/ETC1A4 native uploaded before registration | allowed immediately |

`d9_memory_used()` is now the exact sum of buffer bytes plus cache-accounted
guest and GPU bytes. `d9_texture_resource_info()` exposes pool, usage, format,
levels, split byte costs, and CPU-only/GPU-only flags.

`d9_create_texture_ex()` is the pool-aware API; the existing
`d9_create_texture[_levels]()` wrappers still select DEFAULT. The `disposable`
flag is recorded but **does not drop the guest source yet**.

## Ownership and failure contract

* Device creation initializes a fixed-entry `D9TexCache` with guest-free and
  backend-native release adapters.
* Guest-backed creation registers the guest immediately and attaches the
  preallocated native as pending. Lock pins the entry; writable unlock commits
  the pending native after a successful upload. Readonly, CPU-only, and repeat
  uploads abort/unpin without changing ownership.
* Upload failure keeps the lock and pin so the same unlock can be retried.
* `d9_release_texture()` clears the core slot only after cache removal succeeds.
  A backend release failure returns `D9_RELEASE_FAILED`; already-released copies
  are removed from accounting and the remaining copy can be retried without a
  double free.
* `d9_destroy_device_ex()` reports the first release failure and preserves the
  device for retry. The legacy void wrapper remains best effort.
* Backends may supply optional `texture_release`, `texture_create_encoded`, and
  `texture_upload_encoded` callbacks. Existing six-field backend initializers
  remain valid; PICA implements the encoded ETC1/ETC1A4 callbacks.

Direct SKTX creation validates the container and mip chain **before** GPU
allocation. Any upload failure deletes the partial native and records no bytes.
The cache stores only the native pointer, GPU cost, and optional opaque
rehydrate token; it never retains the temporary SKTX buffer.

## COM/state correctness fixes

* `CreateTexture` passes pool and usage into the core. SYSTEMMEM is CPU-only;
  MANAGED keeps a guest-backed restoration source.
* Textured draws reject declarations without texcoord0 instead of fabricating
  `(0,0)` UVs.
* `SetVertexShaderConstantF/GetVertexShaderConstantF` reject starts outside
  c0..c3, so c4 cannot corrupt the four-constant mask.
* Defaults now match D3D9: WRAP, POINT min/mag, CULL_CCW, and alpha LESS.
* Non-default viewports are explicitly rejected; `SetRenderTarget` no longer
  resets the viewport.
* Rectangle clears and multiple queued `Clear` calls are explicitly rejected
  rather than silently discarded.

## Explicitly pending

* Full MANAGED eviction/rehydrate policy and asset-loader token plumbing.
* Reset preservation/recreation of MANAGED handles.
* Dynamic DISCARD semantics.
* Automatic disposable guest drop after the full-mip/readback contract exists.
* Non-default viewport and rectangle/multiple-clear carry-through.

No reduced-RAM claim is made for DEFAULT or MANAGED: both still retain their
guest copies. Only the direct SKTX path is GPU-only.

## Verification

* Full shared host suite: **38 passed, 1 expected TESV diagnostic, 0 failed**.
* `texture_residency_upload`: **286 checks**; `texture_runtime`: **108 checks**.
* Focused graphics/COM/texture tests pass under ASan/UBSan.
* Isolated ARM graphics (`texresgfx`), COM (`texrescom`), and deep TESV
  (`texres`, growth18/gen03, incremental final rebuild) builds pass.
* Graphics, COM, and TESV source lists include both texture modules.
