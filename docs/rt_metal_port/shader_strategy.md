# Metal RT shader-lowering strategy (C7)

Status: **Available** (per the status vocabulary in `mac-rt-planning/README.md`).

Chunk C7 answers one question with compile-and-run evidence: **can the existing
SPIRV-Cross+MSL lane lower the fork's ray-tracing shaders, or does the Metal
port need its own lowering lane?** The answer is split, and the strategy below
follows the split.

All evidence comes from the repeatable spike in
`tests/drivers/metal/test_metal_rt_shader_strategy.cpp`, which drives the
prototype lane in `drivers/metal/metal_rt_shader_lowering.{h,cpp}`. The CPU
experiments run in the normal unit suite; the two `[MetalRT][GPU]` experiments
run through the canonical runner:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py --stage gpu \
  --binary bin/godot.macos.editor.arm64
```

## What the fork's RT shaders actually are

`servers/rendering/renderer_rd/shaders/raytracing/scene_raytracing_raygen.glsl`
contains all five Vulkan RT-pipeline stages (`raygen`, `miss`, `closest_hit`,
`any_hit`, `intersection`) built around `traceRayEXT`, ray payloads, and hit
attributes. The include tree also uses `GL_EXT_ray_query` directly
(`raytracing_closest_hit_common_inc.glsl`, `raytracing_lights_inc.glsl`).
Shader Execution Reordering (`hitObjectEXT`, `USE_SER`) is an optional,
macro-gated NVIDIA path. There are no callable stages and no image atomics in
the RT shader tree, and the fork dispatches all RT work from a dedicated pass,
never from raster stages.

## Measured results (Apple M5, macOS 26.5, 2026-07-13)

| # | Experiment | Result |
|---|---|---|
| 1 | Ray-query compute GLSL → glslang → SPIRV-Cross MSL 2.4, classic bindings | **Lowers** to `metal::raytracing::intersection_query`; entry `main0` |
| 2 | Same SPIR-V, tier-2 argument buffers, `pad_argument_buffer_resources` **on** (the container's exact configuration) | **Lowered after C9**: the vendored padded-binding lookup now treats acceleration structures as buffer-index resources |
| 3 | Same SPIR-V, tier-2 argument buffers, padding **off** | **Lowers** cleanly |
| 4 | Lowered kernel from #1 compiled with `newLibraryWithSource` (MSL 2.4) and dispatched against the C5/C6 BLAS+TLAS | **Correct**: hit at t=2.0, primitive 0, instance 0; miss ray misses |
| 5 | Raygen stage (`traceRayEXT`) → SPIRV-Cross MSL | **Fails**: "A memory declaration object must be used in TraceRayKHR." |
| 6 | Closest-hit stage (payload + hit attributes) → SPIRV-Cross MSL | **Fails**: "PrimitiveId is not supported in this execution model." |
| 7 | Handwritten `metal::raytracing::intersector` kernel at MSL **2.3** (macOS 11 floor), same scene and rays | **Correct**: identical hit/miss results |
| 8 | Production B2 rewrite applied to SPIRV-Cross-shaped MSL, specialized as query and `ALL_OPAQUE` intersector from one library | **Correct**: identical closest-hit, shadow, miss, `t_min`, and `t_max` results |

Two structural facts back up #5/#6: the vendored SPIRV-Cross MSL backend maps
every RT execution model to entry type `"unknown"` (`spirv_msl.cpp`,
`func_type_decl`), and it implements no RT-pipeline opcodes or storage classes.
Extending it would mean building a new backend inside SPIRV-Cross, not patching
one.

## Strategy

**Compute-only ray tracing with two lanes, and no mechanical lowering of
RT-pipeline stages.**

1. **Lane A — existing SPIRV-Cross container lane** for everything that is a
   compute stage, including `GL_EXT_ray_query` tracing. Proven end-to-end by
   experiments #1/#3/#4. Requirements this creates for C8/C9:
   - C9 taught `RenderingShaderContainerMetal`
     `UNIFORM_TYPE_ACCELERATION_STRUCTURE` for classic and argument-buffer
     bindings.
   - C9 resolved the argument-buffer padding gap with the narrow vendored
     SPIRV-Cross switch addition: `SPIRType::AccelerationStructure` uses the
     buffer-index namespace. Padding remains enabled for RT kernels.
   - MSL floor for `intersection_query` is 2.4 (macOS 12); the effective
     RT floor stays macOS 13 per assumption A4, so this adds no constraint.

2. **Lane B — native MSL** for the backend-owned trace skeleton. The C8
   "trace one ray" kernel is authored directly in MSL against
   `metal::raytracing::intersector`, proven at MSL 2.3 by experiment #7.
   Backend-owned kernels do not round-trip through SPIR-V at all.

   The production compute scene additionally has a bounded B2 specialization:
   `MetalRTShaderLowering::patch_scene_ray_query_to_intersector()` injects
   native closest-hit and shadow helpers into SPIRV-Cross output. It engages
   only for `RT_FLAG_ALL_OPAQUE`; alpha/custom variants retain the original
   query body. The patch is transactional and returns a stable status code for
   every intentional exclusion or output-anchor mismatch. Tracked MSL fixtures
   and experiment #8 prevent a SPIRV-Cross format drift from silently removing
   or semantically changing this lane.

3. **RT-pipeline stages are re-expressed, not translated.** The five-stage
   `traceRayEXT` program cannot be pushed through SPIRV-Cross (experiments
   #5/#6). The path-tracer integration (C10) therefore converts the
   raygen/miss/closest-hit control flow into a **ray-query compute kernel**:
   the raygen loop becomes the kernel body, `traceRayEXT` becomes a ray query
   (or, if profiling justifies it later, a Lane-B intersector kernel calling
   SPIRV-Cross-compiled visible functions), and miss/hit logic is inlined at
   the call site. This keeps material evaluation and lighting code in GLSL on
   Lane A unchanged; the fork's own DLSS-RR path already traces via ray
   queries inside these includes, so the conversion has precedent in-tree.
   - `traceRayEXT`'s SBT indexing collapses: with one inlined hit path there
     is no shader-group table to index. `hit_sbt_offset`/instance
     `gl_InstanceCustomIndexEXT` semantics must be carried explicitly (the C6
     instance record already retains both driver-side).
   - SER (`USE_SER`) has no Metal equivalent and stays compiled out.

### Why not the alternatives

- **Extend SPIRV-Cross with RT execution models**: a new-backend-sized effort
  inside a vendored thirdparty library, ongoing merge burden, no upstream
  appetite signal. Rejected.
- **Author the whole path tracer natively in MSL**: forks every material/BSDF
  include into a second language and loses the single-authoring-IR property
  that the rest of the engine relies on. Rejected; native MSL is reserved for
  the small backend-owned skeleton.

## Consequences for the assumption register

- **A7 is resolved as split**: the SPIRV-Cross lane *is* adequate for
  ray-query compute (falsifying the blanket "cannot lower RT SPIR-V"), and is
  confirmed inadequate for RT-pipeline stages. `assumptions.md` is updated.
- **A5 (compute-pipeline-only) is reinforced**: both proven lanes are compute.
- **A4 (macOS 13 effective floor) is unchanged**: Lane B works at macOS 11's
  MSL 2.3; Lane A ray queries need MSL 2.4 (macOS 12); argument buffers keep
  the effective floor at 13.
- Open question 2 (64-bit image atomics) is now closed: the RT shader tree
  contains no image atomics.
