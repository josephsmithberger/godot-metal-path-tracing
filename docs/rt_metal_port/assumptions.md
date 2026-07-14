# Metal RT port assumptions (chunk C2)

Status: **Available** (per the status vocabulary in `mac-rt-planning/README.md`).

This document records the planning assumptions the Metal ray tracing port is
built on, why each one is believed, and how to falsify it. It accompanies
[`capability_matrix.json`](capability_matrix.json), which holds the feature
inventory these assumptions gate, and builds on the frozen ground truth in
[`baseline.md`](baseline.md).

Verification evidence for this chunk comes from the standalone capability
probe:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py --stage caps
```

The probe (`capability_probe.mm`) compiles with only Foundation and Metal,
writes a JSON capability record satisfying the "GPU functional contract" in
`mac-rt-planning/testing-standard.md`, and exits nonzero if any measured
capability contradicts the family expectations in the matrix. First verified
run: Apple M5 (`Mac17,2`), macOS 26.5.2, 2026-07-13 — 5/5 checks passed, all
ray-tracing capabilities present.

## Assumption register

| ID | Assumption | Confidence | Falsified by |
|---|---|---|---|
| A1 | Shipping scope is Apple Silicon arm64 only | High | A product decision to support Intel Macs |
| A2 | Runtime device queries, not GPU families, gate the RT path | High | Apple removing/deprecating the query surface |
| A3 | Every required RT API exists at the arm64 build floor, macOS 11.0 | High | Compile error against an older SDK path in C4-C6 |
| A4 | The effective runtime floor will be macOS 13.0 (encoder-free tier-2 argument buffers + gpuAddress) | High (C9 kept padded tier-2 argument buffers) | C10 finding a lower-floor binding design |
| A5 | First scope is compute-pipeline ray tracing only | High (reinforced by C7: both proven lanes are compute) | C8 finding a hard dependency on render-stage RT |
| A6 | Vulkan SBT handles map to function-table indices | Resolved by C9 | A future native Metal RT-pipeline model |
| A7 | ~~The existing SPIRV-Cross lane cannot lower RT-stage SPIR-V~~ **Resolved by C7 as split**: the lane lowers ray-query compute; RT-pipeline stages are confirmed impossible and get re-expressed as compute | Resolved | A vendored SPIRV-Cross update gaining RT execution models |
| A8 | Unified memory holds on all target devices; shared storage mode is acceptable for bring-up | High | Probe reporting `has_unified_memory: false` on a target device |
| A9 | One dev machine (Apple M5) covers both RT hardware lanes for bring-up; M1/M2 coverage is deferred | Medium | Family-dependent bug reports on apple7/apple8 |
| A10 | DLSS/Streamline stays out of scope; no upscaler is part of this port | High | A scope change |

## A1: Apple Silicon arm64 only

The build system already refuses to enable the Metal driver on any other
architecture (`platform/macos/detect.py:309-311`), and Intel Macs cannot gain
it without new build plumbing. Some Intel-era AMD GPUs do report
`supportsRaytracing` under the `mac2` family, but they are unreachable by the
engine's Metal backend, so they impose no requirements on this port.

*Consequence:* the probe reports `engine_metal_driver_enabled_for_arch` and the
Rosetta translation state so that a mis-arched test binary is detectable, per
the `unsupported_arch` skip contract.

*Falsify/re-verify:* `grep -n "does not support the Metal rendering driver"
platform/macos/detect.py`.

## A2: Runtime queries are the gating authority

Apple's feature tables describe families, but the API exposes per-device
booleans (`supportsRaytracing`, `supportsFunctionPointers`,
`supportsRaytracingFromRender`, `argumentBuffersSupport`). The engine's
existing feature layer (`drivers/metal/metal_device_properties.cpp`) mixes
both; for RT the port must gate on the queries and use families only for
expectations and performance tiers. Two facts from the first probe run force
this: the M5 reports family `apple10` (raw 1010), which the engine's family
scan does not know (it caps at `apple9`,
`metal_device_properties.cpp:109-115`), and hardware-RT presence (apple9+) has
no dedicated query at all.

*Resolved in C11:* `SUPPORTS_RAY_QUERY` is derived from runtime capability
queries plus the binding-model and OS-floor checks in
[`runtime_gating.md`](runtime_gating.md), never from a family comparison.
`SUPPORTS_RAYTRACING_PIPELINE` remains false because Metal executes the port
through a re-expressed compute lane rather than the engine's five RT stages.

*Falsify/re-verify:* run the caps stage; the probe cross-checks queries against
family expectations and fails loudly on divergence.

## A3: API floor macOS 11.0 for the core RT surface

`MTLAccelerationStructure`, the acceleration-structure command encoder
(build/refit/copy-compact), visible/intersection function tables, and the MSL
2.3 `intersector` intrinsics all shipped in macOS 11.0 — the same version as
the arm64 deployment target (`platform/macos/detect.py:109-111`). So C4-C6
(skeleton, BLAS, TLAS) can be written against the deployment target without
new availability guards, using the macOS 11 descriptor-array TLAS path
(`instancedAccelerationStructures`) rather than macOS 13 resource-ID
instances.

*Falsify/re-verify:* any `@available` warning or missing-symbol error while
implementing C4-C6 against `-mmacosx-version-min=11.0`.

## A4: Effective runtime floor is expected to be macOS 13.0

Two engine mechanisms the path tracer's bindless design leans on are gated at
macOS 13 in the existing capability layer:

- encoder-free tier-2 argument buffers: `needs_arg_encoders` is only cleared on
  macOS 13+ with the Metal3 family
  (`metal_device_properties.cpp:173-175`), and
  `argument_buffers_supported()` requires that;
- raw buffer GPU addresses: `supports_gpu_address` is macOS 13+
  (`metal_device_properties.cpp:132-134`).

The NVIDIA path tracer accesses scene resources through bindless blocks
(`servers/rendering/renderer_rd/bindless_block.{h,cpp}`), so the Metal RT path
will very likely require both. Until C7 proves otherwise, plan for: **build
floor macOS 11 (unchanged), RT-enabled floor macOS 13**, reported as a
graceful capability fallback (C11), not a crash.

*Consequence:* C4-C6 must not silently depend on macOS 13 APIs — the AS layer
itself works at 11.0 (A3); only the shader-facing binding model raises the
floor.

*Falsify/re-verify:* the C7 spike either produces a working RT kernel with
argument encoders (floor drops back toward 11.0) or confirms the dependency
(floor is 13.0 and C11 encodes it).

## A5: Compute-pipeline ray tracing only

Metal has no standalone ray-tracing pipeline object; the natural mapping for
`command_trace_rays` is a compute dispatch whose raygen kernel uses
`metal::raytracing::intersector` and calls hit/miss logic through visible
function tables. Ray tracing from render stages
(`supportsRaytracingFromRender`, macOS 12+) is not needed to reach path-tracer
parity, because the fork dispatches its RT work from a dedicated pass
(`render_raytracing.cpp`), not from raster shaders.

*Falsify/re-verify:* C7/C8 uncovering a fork shader that traces rays from a
raster stage.

## A6: SBT handles become function-table indices

`raytracing_pipeline_get_shader_group_handles` exists because Vulkan returns
opaque shader-group handles that the engine copies into a shader binding
table. Metal's analog is an index into a `MTLVisibleFunctionTable` /
`MTLIntersectionFunctionTable`. The plan is to return small fixed-size records
containing table indices and have the trace path resolve them, documenting
every place the SBT semantics (stride, offset arithmetic) do not map 1:1.
**Resolved by C9.** Metal returns engine-defined 16-byte records containing the
stable group index, group type, and intersection-function-table index. Group
order is raygen, miss, then hit. Triangle hit groups share the system opaque
triangle function at slot 0; procedural groups reserve stable slots; raygen,
miss, and empty groups have no intersection-table index. See
[`pipeline_mapping.md`](pipeline_mapping.md) for every non-1:1 rule.

*Re-verify:* `--test-case="*[MetalRT] C9*"` creates a pipeline from synthetic
shader groups and validates exact record bytes, stable indices, table entries,
and bind order.

## A7: RT shader stages need a Metal-specific lowering lane (resolved by C7)

The existing shader container compiles SPIR-V to MSL via SPIRV-Cross
(`rendering_shader_container_metal.cpp`), which is proven for raster and
compute. It was assumed **insufficient** for the fork's RT-stage SPIR-V
(raygen/miss/closest-hit from `scene_raytracing_raygen.glsl` and friends),
because SPIRV-Cross's MSL backend does not translate Vulkan RT-pipeline
execution models.

**C7 resolved this as a split** (see
[`shader_strategy.md`](shader_strategy.md) for the full evidence table):

- Ray-query compute SPIR-V lowers, compiles, and traces correctly through the
  existing lane (verified on-device against the C5/C6 acceleration
  structures), so the blanket form of A7 is falsified.
- RT-pipeline stages are confirmed impossible: raygen fails in SPIRV-Cross
  with "A memory declaration object must be used in TraceRayKHR." and
  closest-hit with "PrimitiveId is not supported in this execution model."
  They are re-expressed as ray-query compute kernels, not translated.
- One real defect in the existing lane was measured: with
  `pad_argument_buffer_resources` enabled (the container's configuration),
  SPIRV-Cross rejected acceleration-structure bindings. C9 fixed the narrow
  vendored lookup by treating them as buffer-index resources; padding remains
  enabled.

*Re-verify:* `--test-case="*[MetalRT] C7*"` pins every result above; the GPU
half runs in the runner's `gpu` stage.

## A8: Unified memory and shared storage for bring-up

All Apple Silicon devices report `hasUnifiedMemory` (verified on M5). Scratch
buffers, instance buffers, and readback surfaces can use shared storage during
bring-up without a staging path; private-storage optimization is deferred
until after correctness (L4/L5) is stable.

*Falsify/re-verify:* probe record from any target device with
`has_unified_memory: false`.

## A9: Hardware lanes covered by this machine, with a known gap

The testing standard's "RT baseline" lane requires apple6+ and "RT advanced"
requires apple9+. The verified M5 (family raw 1010 > apple9) satisfies both,
so all local L4+ development can proceed on this machine. It cannot detect
issues specific to pre-hardware-RT devices (M1 = apple7, M2 = apple8), where
the same API runs on a software/shader-based intersector with different
performance and potentially different edge-case behavior.

*Consequence:* C11's gate and forced fallback are repeatably tested on the M5,
but at least one probe record and smoke run from an apple7 or apple8 device
should still be collected before claiming cross-family performance and
edge-case coverage; current GPU results generalize only to apple9+.

*Falsify/re-verify:* run the caps stage on an M1/M2 machine and diff the
record against the M5 one.

## A10: No upscaler in the RT port

Streamline/DLSS is Windows-only in this fork (`SConstruct:608-634`) and stays
out of scope, per the baseline. MetalFX upscaling exists in the backend
(`metal_fx_spatial/temporal` in `MetalFeatures`) and may pair with the path
tracer later, but no chunk C1-C12 depends on it.

*Falsify/re-verify:* scope change from the project owner.

## Resolved implementation decisions

- **RT backend layer (resolved in C4).** The acceleration-structure and
  pipeline skeletons live in the shared `RenderingDeviceDriverMetal` base.
  Core Metal RT descriptors and size queries are available at the macOS 11
  build floor and do not depend on Metal 3 residency sets or newer encoders;
  keeping them in the base also replaces the 14 stubs at their owning layer.
- **SBT and recursion mapping (resolved in C9).** Shader-group handles are
  stable 16-byte Metal index records, not native function pointers. The
  requested recursion depth is stored as a software budget because Metal has
  no corresponding pipeline property; the compute-lane kernel enforces it as
  its bounce budget (C10).
- **Path-tracer launch path (resolved in C10).** Ray tracing dispatches as a
  compute-lane pipeline whose raygen is a re-expressed ray-query compute
  kernel; `command_trace_rays` performs the grid dispatch and the
  compatibility SBT is not consumed at trace time. Instance custom indices are
  shader-visible through Metal UserID instance descriptors (macOS 12+; within
  the A4 floor). C11 exposes this lane through the runtime checks and graceful
  fallback in [`runtime_gating.md`](runtime_gating.md). C13 adds the restricted
  HG0 `SceneShaderRaytracing` integration; its exact subset and remaining
  first editor subset is documented in [`editor_hg0.md`](editor_hg0.md), and
  C14 geometry/lifetime widening in [`scene_geometry.md`](scene_geometry.md).

## Open questions (tracked, not assumed)

1. **64-bit image atomics.** ~~Whether the path tracer's accumulation targets
   need `supports_image_atomic_64_bit` (apple9-or-apple8+mac2 only) is unknown
   until the C7 shader audit.~~ Closed by the C7 audit: the RT shader tree
   under `servers/rendering/renderer_rd/shaders/raytracing/` contains no image
   atomics, so no lane fragmentation on apple7/apple8.
2. **Family scan staleness.** The engine's highest-family scan stops at
   apple9; M5 reports apple10. Nothing RT-critical keys off the exact family
   today, but any future family-based tiering (A2) must use `>=` comparisons,
   not equality, and the scan ceiling should be raised opportunistically.
