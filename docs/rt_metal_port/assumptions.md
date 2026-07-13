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
python3 mac-rt-planning/scripts/run_mac_rt_tests.py --stage caps
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
| A4 | The effective runtime floor will be macOS 13.0 (encoder-free tier-2 argument buffers + gpuAddress) | Medium | C7 shader-strategy spike |
| A5 | First scope is compute-pipeline ray tracing only | High | C7/C8 finding a hard dependency on render-stage RT |
| A6 | Vulkan SBT handles map to function-table indices | Medium | C9 pipeline-mapping implementation |
| A7 | The existing SPIRV-Cross lane cannot lower RT-stage SPIR-V; a Metal-specific lane is needed | Medium | C7 compile experiment |
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

*Consequence for C11:* `SUPPORTS_RAYTRACING_PIPELINE` should be derived from
`supportsRaytracing && supportsFunctionPointers` (plus the OS floor from A4),
never from a family comparison.

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
This is a design assumption for C9, recorded here so that C4's skeleton
reserves the right data shapes.

*Falsify/re-verify:* C9 unit test creating a pipeline from synthetic shader
groups and validating stable indices and bind order.

## A7: RT shader stages need a Metal-specific lowering lane

The existing shader container compiles SPIR-V to MSL via SPIRV-Cross
(`rendering_shader_container_metal.cpp`), which is proven for raster and
compute. It is assumed **insufficient** for the fork's RT-stage SPIR-V
(raygen/miss/closest-hit from `scene_raytracing_raygen.glsl` and friends),
because SPIRV-Cross's MSL backend does not translate Vulkan RT-pipeline
execution models. The C7 spike decides between: extending the SPIRV-Cross
lane, authoring the Metal RT kernels natively, or a hybrid (SPIR-V for
material evaluation, native MSL for the trace skeleton).

*Falsify/re-verify:* C7 attempts to run one tiny RT shader through the
existing container and records exactly where it breaks (or does not).

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

*Consequence:* before declaring C11 (gating/fallback) done, at least one probe
record and one smoke run from an apple7 or apple8 device should be collected;
until then results generalize only to apple9+.

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

## Open questions (tracked, not assumed)

1. **Trace recursion depth.** `raytracing_pipeline_create` receives
   `p_max_trace_recursion_depth`; Metal's intersector model has no pipeline
   recursion limit — recursion becomes an in-kernel loop. Where the engine
   relies on Vulkan's `maxRayRecursionDepth`, the Metal driver must pick and
   report an honest equivalent. Decide in C9.
2. **64-bit image atomics.** Whether the path tracer's accumulation targets
   need `supports_image_atomic_64_bit` (apple9-or-apple8+mac2 only) is unknown
   until the C7 shader audit. If yes, it fragments the apple7/apple8 lane.
3. **Family scan staleness.** The engine's highest-family scan stops at
   apple9; M5 reports apple10. Nothing RT-critical keys off the exact family
   today, but any future family-based tiering (A2) must use `>=` comparisons,
   not equality, and the scan ceiling should be raised opportunistically.
