# Metal alpha and custom material dispatch (C15)

Status: **Available** for the E2 material matrix. Human review of alpha edges,
two-sided shading, texture sampling, and live editor updates is still required.
C16 subsequently reuses this dispatch identity for procedural intersections.

C15 uses generated/inlined material variants for the Metal compute ray-query
lane. A Metal visible-function table was not selected: the existing Godot
spatial compiler already produces stage bodies and uniform layouts, while one
generated compute kernel keeps all scene bindings identical to HG0 and avoids
function-table lifetime references during live reload. Vulkan retains its
native hit-group pipeline.

## Dispatch and cache identity

Custom shader sources are preprocessed into fragment/vertex bodies, an std140
uniform layout, bindless texture-index members, and alpha-clip metadata. Each
distinct dual-64-bit source hash receives an append-only dispatch slot. The
generated compute shader contains one evaluator function per accepted slot and
a `MaterialData.dispatch_index` switch; slot zero remains the built-in HG0
evaluator.

The compiled-variant key is `(sanitized_rt_flags, material_generation)`.
Unsupported Metal feature bits are removed before lookup; sample and bounce
specialization bits remain. Adding a distinct accepted source advances the
generation. Runtime material records have a separate cache key consisting of
RID version plus the material invalidation counter, so uniform, texture, and
standard-material edits rebuild their GPU record without recompiling unchanged
shader source.

Successful publications emit cumulative compile, rejection, and cache-hit
statistics:

```text
Metal RT material variant: status=compiled flags=0x... generation=... active_custom=... compiles=... failures=... cache_hits=...
```

## Material data mapping

| Spatial material input | Metal compute representation |
|---|---|
| Built-in albedo, normal, ORM, emission, UV transform, filtering | HG0 fields plus bindless texture indices |
| Alpha scissor | Alpha and threshold evaluated before `rayQueryConfirmIntersectionEXT`; the same path is used for shadow visibility |
| Cull mode | TLAS facing-cull/flip flags; disabled culling admits both faces |
| Custom scalar/vector/matrix uniforms | Initialized, address-stable std140 buffer-reference record; Vulkan may suballocate the shared pool, while Metal uses a dedicated record so address-only reads participate in safe lifetime tracking |
| Custom sampler uniforms | Bindless texture indices appended to that record, preserving source-color and default-texture metadata |
| Shader specialization | Generated source slot plus the sanitized scene specialization flags |
| Material identity | Stable RID index in `MaterialData.material_id`, exposed by the Material ID debug view |

## Device-address residency

The compute lane reads geometry (vertex/attribute/index) and material data
through raw GPU buffer addresses. Metal only guarantees residency for
resources an encoder binds, so the driver now tracks every buffer whose
device address is queried: on OSes with `MTLResidencySet` support these
buffers join a queue-level set once, and otherwise they are marked with
`useResources` on any compute encoder that binds an acceleration structure.
Without this, alpha-test candidate evaluation dereferenced non-resident
buffers (Metal shader validation: `Invalid device load ... resident:No`),
producing per-frame garbage alpha tests, missing cutout holes, corrupted
textures after pipeline swaps, and eventual GPU faults that hang
`waitUntilCompleted`.

## Ray flags

`trace_material` keeps `RT_RAY_FLAGS` (back-face culling) so the compute lane
matches the Vulkan raygen lane; double-sided materials override culling per
instance via `ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT`.
Shadow visibility uses a dedicated `trace_shadow_blocked` query with
`gl_RayFlagsTerminateOnFirstHitEXT`, which stops at the first alpha-accepted
candidate instead of resolving the closest hit.

Custom source reload is append-only. At the render-frame boundary, pending
sources are compiled into a trial monolithic variant. A successful variant and
its pipeline/SBT are swapped together; the rendering device defers old RID
destruction until submitted work is finished. Existing surfaces remain
excluded for the single warm-up frame rather than being rendered through the
wrong HG0 semantics.

## Failure and fallback contract

Opaque built-in materials may safely use HG0. A custom shader is never silently
substituted with HG0. Preprocess or generated-variant failure gives the source a
stable failed slot, omits affected surfaces from the TLAS, retains the previous
published variant, and logs how to retry by editing the shader.

C15 accepts direct custom vertex/fragment bodies with ordinary uniforms and
2D textures. Stage-global custom helper functions need cross-material symbol
namespacing and are deliberately rejected with an actionable diagnostic.
Transparent blending, alpha-to-coverage/hash parity, screen/depth/normal
texture reads, native denoising, and SER are not represented by this contract.
Procedural AABBs are now covered separately by
[`procedural_geometry.md`](procedural_geometry.md).

## Verification

Focused `[MetalRT]` tests check the 112-byte material ABI, alpha/custom flag
separation, RID/counter invalidation, generated-variant generation keys, and
uniform/texture buffer bounds. The E2 editor fixture adds opaque,
alpha-scissored, double-sided, textured, and custom uniform/texture surfaces;
changes shader source, uniforms, and alpha threshold in-process; records
beauty and Material ID views; and includes a helper-function shader that must
be excluded.

Run the focused unit tests and the automated E2 stage with:

```bash
./bin/godot.macos.editor.arm64 --test '--test-case=*[MetalRT]*' --force-colors

python3 tests/metal_rt/run_mac_rt_tests.py --stage material-scene \
  --binary bin/godot.macos.editor.arm64
```

The `material-scene` stage performs the cold and reload captures, runs
`tests/metal_rt/verify_material_scene.py` against them, and repeats the
capture under `GODOT_MTL_DISABLE_RAYTRACING=1` to prove the raster fallback.
Like every RT-dispatching stage it runs under Metal shader validation and
fails on any invalid device load or store (see `ci_validation.md`). The
required engine/fixture markers are:

```text
METAL_RT_C15_MATERIAL_DISPATCH=passed
METAL_RT_ALPHA_TEST=passed
METAL_RT_CUSTOM_SHADER_RELOAD=passed
```

## Manual acceptance

Open `tests/metal_rt/editor/fixtures/e2_materials.tscn` in the Path Tracing
view. Confirm that the alpha card has clean holes and casts a cutout shadow,
the reversed double-sided card remains visible with plausible lighting, the
checker texture is oriented correctly, and the custom box initially has a
purple contribution. After the scripted edit, the custom box should become
bright cyan and the alpha silhouette should tighten without a black frame,
stale purple flash, or disappearing unrelated surfaces.

Switch to Material ID and confirm stable, distinct colors for the floor and
five supported materials before and after reload. The intentionally rejected
helper box at the far right must remain absent from the path-traced result and
produce the documented diagnostic, while raster fallback remains usable.
Finally, orbit the camera and repeat source/uniform edits to look for flicker,
stale dispatch, GPU-validation messages, or memory growth. Those interactive
appearance and long-session observations cannot be approved by the automated
checks.
