# Path-tracer scene launch on the Metal compute lane (C10)

Status: **Available** for the driver/RenderingDevice trace path, the controlled
scene evidence, and the restricted C13 HG0 editor scene. See
[`editor_hg0.md`](editor_hg0.md) for the real-scene contract and remaining
material/geometry exclusions.

Chunk C10 makes the Metal backend launch a path-traced scene through Godot's
own raytracing abstractions. It implements the compute-lane design selected in
C7 and mapped in C9: the ray-generation program is a **re-expressed ray-query
compute kernel** compiled through the regular shader lane, and
`command_trace_rays` dispatches it as a compute grid.

## What C10 adds

### User-ID instance descriptors

`rayQueryGetIntersectionInstanceCustomIndexEXT` lowers to MSL
`user_instance_id`, which Metal only populates from
`MTLAccelerationStructureUserIDInstanceDescriptor` records. The C6 instance
record now uses that 68-byte native prefix (identical to the default
descriptor for its first 64 bytes) and `tlas_create()` selects the UserID
descriptor type on macOS 12+. This closes the C8 gap "C10 must expose instance
metadata to the re-expressed compute shader": Godot's instance custom index —
the path tracer's geometry index — is now shader-visible.

- At the macOS 11 build floor the field compiles and stays CPU-side metadata;
  the shader-visible path needs macOS 12+, below the effective macOS 13 RT
  floor from assumption A4, so it adds no constraint.

### Compute-lane raytracing pipelines

`RenderingDevice::raytracing_pipeline_create` accepts a **compute-stage**
ray-generation shader when the driver reports `SUPPORTS_RAY_QUERY` without
`SUPPORTS_RAYTRACING_PIPELINE`. The Metal driver then builds the pipeline
state exactly like a compute pipeline — including specialization constants,
so `RT_FLAGS` versioning keeps working — and stores it on
`MDRaytracingPipeline`.

Compute-lane group rules (validated by `configure_shader_groups`):

| Group | Rule |
|---|---|
| Raygen | Exactly one, compute stage: the kernel is monolithic |
| Miss | None: miss logic is inlined at the trace call site |
| Hit | Empty sentinel records only: hit logic is inlined; records keep stable indices for instance `hit_sbt_offset` resolution |

Ray-query kernels use no intersection-function table. This was the C10
boundary; C16 later represents procedural hit groups by inlining their custom
intersection bodies into the generated compute kernel, as documented in
[`procedural_geometry.md`](procedural_geometry.md).

### Trace dispatch

`command_trace_rays` now dispatches the bound compute-lane pipeline over the
requested pixel grid (threadgroups round up; the kernel bounds-checks). The
compatibility SBT buffers are intentionally not consumed: with inlined hit
logic there is no shader-group table to index at trace time, which is the
"SBT indexing collapses" consequence documented in `shader_strategy.md`.
Uniform sets, including the TLAS, bind through the existing compute state.

TLAS residency is completed at bind time: `prepare_tlas_build()` records the
unique referenced BLASes and both uniform-bind paths (`setAccelerationStructure`
and tier-2 argument buffers) mark them resident with `useResources`. With
Metal residency sets enabled, acceleration structures are already tracked
globally and no per-dispatch call is issued.

### Runtime exposure

C11 replaced the default-off debug exposure with the capability gate documented
in [`runtime_gating.md`](runtime_gating.md).
`rendering/pathtracer/metal_ray_query_backend` now defaults on, but the Metal
driver reports `SUPPORTS_RAY_QUERY` only when every compute-lane and bindless
requirement is available. `SUPPORTS_RAYTRACING_PIPELINE` stays false. C13 gives
`SceneShaderRaytracing` a separate compute bundle and enables the scene route
only when that bundle reports ready; native Vulkan RT-pipeline selection is
unchanged.

### Bindless material-access foundation

The Metal shader container now preserves runtime-sized texture arrays as
unbounded bindings instead of treating them as fixed one-element arrays. Such
bindings are accepted only as the final binding in a descriptor set and only
with tier-2 argument buffers; SPIRV-Cross then receives a zero descriptor count
and emits the argument buffer in the device address space. Uniform-set creation
sizes the trailing descriptor region from the set's actual texture count.

`tests/drivers/metal/test_metal_rt_shader_strategy.cpp` exercises the real
container with the material-access pattern needed by the scene shaders: a
GPU-addressed material record selects a nonuniform entry from an unbounded
texture array. Its GPU case binds two descriptors and verifies that material
index 1 returns the second texture's color. The focused sizing case in
`tests/drivers/metal/test_metal_rt.cpp` verifies that the allocated argument
buffer grows with the runtime descriptor count.

## Controlled scene evidence

`tests/drivers/metal/test_metal_rt_pathtracer_launch.cpp` launches an
8x8-pixel, 2-samples-per-pixel, two-bounce path-traced scene:

- floor and shadow-blocker quads in two BLASes, two TLAS instances carrying
  user IDs 7 and 3 and distinct 8-bit masks;
- the kernel re-expresses the `scene_raytracing_raygen.glsl` control flow:
  the raygen loop is the kernel body, `traceRayEXT` is a ray query, miss/hit
  logic is inlined, next-event-estimation shadow rays use an occlusion query,
  and the RNG (`pcg_hash`/`init_rng`) plus `offset_ray_origin` are taken
  verbatim from `raytracing_inc.glsl`. The `PathPayload` packing does not
  survive re-expression by design: it exists to cross RT-pipeline stage
  boundaries, which the compute lane eliminates;
- the kernel is compiled through the real lane (glslang → SPIRV-Cross MSL 2.4)
  and reads the committed instance's user ID to index materials — a wrong
  custom index floods the image with a magenta canary color;
- the image is compared numerically against a CPU reference (max channel
  difference below 0.02; hit/miss/shadow classification edges keep >=0.125
  world units of margin from every pixel sample so the classification is
  deterministic across CPU/GPU float differences);
- both the GPU image and the CPU reference are written as PNG artifacts into
  the runner's stage directory via `GODOT_MRT_ARTIFACT_DIR`.

Run the CPU mapping cases:

```bash
./bin/godot.macos.editor.arm64 --test '--test-case=*[MetalRT] C10*' --force-colors
```

Run the GPU launch with the canonical runner (writes the PNG artifacts):

```bash
python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage gpu --binary bin/godot.macos.editor.arm64
```

Its log contains a line beginning:

```text
MetalRT C10 path-tracer launch: device="..." image=8x8 spp=2 bounces=2 instances=2 max_diff=...
```

C12 promotes this output to a reviewed image regression with a committed PNG,
manifest, visual diff, numeric metrics, and self-hosted CI lane. See
[`ci_validation.md`](ci_validation.md), or run only that stage with
`--stage image`.

## Remaining work toward full scene integration

C13 supplies the compute re-expression for opaque, static HG0
`StandardMaterial3D` scenes. Full parity still requires custom hit groups
(HG1+), alpha policy, deformed and instanced geometry lifetime, broader light
and environment coverage, native denoising/presentation, and export coverage.
Those remain separate follow-up chunks so this first editor lane has an honest,
testable boundary.

The C11 runtime gate and fallback are complete; see
[`runtime_gating.md`](runtime_gating.md).
