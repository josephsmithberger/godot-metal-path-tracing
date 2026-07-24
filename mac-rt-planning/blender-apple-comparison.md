# Blender / Apple Metal RT comparison

**Status:** Merge blockers implemented (2026-07-17). B1-B3 and the fast-trace
usage mapping landed on `nvidia-pt-dlss`; see "Implementation status" below.
P1, P2, and P4 are implemented (2026-07-17, second pass); P3's
lowering-metadata foundation is implemented and its alpha/procedural IFT lanes
remain declared-but-pending in the traversal registry.

## P-chunk implementation status (2026-07-17)

- **P1 — implemented.** The Metal compute route no longer compiles one full
  generated kernel per newly ready material slot. `SceneShaderRaytracing` now
  batches every slot that became ready since the last idle frame into a single
  aggregate compile (`ComputeBuildTask`) that runs on a worker thread through a
  dedicated single-lane queue (`compute_compile_lane`). The bundle keeps its
  last-known-good pipeline while the aggregate compiles; the swap happens at
  the frame boundary in `drain_completed_compiles()` with a generation check
  (stale output is dropped, and a generation advance re-marks the bundle
  dirty). If the aggregate fails, a group bisection over the candidate list
  isolates failing slots (marked per-bundle `Failed` with the compiler error)
  and the surviving union is used. Compiled aggregates are cached by
  (compute variant, active slot-index/source-hash set) in
  `compute_variant_cache` with bundle+task refcounts, so the ALL_OPAQUE
  rt_flags bundle reuses the base bundle's kernel instead of recompiling it.
  The HG0 bootstrap stays synchronous (`_build_compute_bundle` builds the
  generic template pipeline only). `rendering/pathtracing/async_shader_compilation=false`
  preserves the fully synchronous single-frame behavior.
- **P2 — implemented.** Full copy-and-compact lifecycle for immutable BLASes.
  Driver: `MDAccelerationStructure::encode_build` now writes the compacted
  size with the explicit 64-bit `sizeDataType` variant (macOS 13+/iOS 16+,
  legacy 32-bit write below), and new RDD entry points
  (`acceleration_structure_get_compacted_size` / `_get_allocated_size`,
  `blas_create_compacted_target`, `command_compact_blas`) expose the query,
  the bare compacted-target allocation, and the copy. The copy is
  graph-visible: `RenderingDeviceGraph::add_blas_compact` records a
  `TYPE_BOTTOM_LEVEL_ACCELERATION_STRUCTURE_COMPACT` command with
  src=READ / dst=READ_WRITE trackers, so it orders before the TLAS build that
  references the destination. Renderer: static BLASes are created with
  `ALLOW_COMPACTION` (gated on the new `SUPPORTS_BLAS_COMPACTION` RD
  feature); the per-frame surface walk revisits pending surfaces, and
  `_process_blas_compactions()` polls the recorded size (nonzero exactly when
  the size-writing build completed), allocates the right-sized target,
  enqueues the compact, swaps `RTSurfaceData::blas` plus this frame's TLAS
  instance list, and defers the source free through RD's per-frame disposal
  (source retained until no submitted work references it). Compactions are
  budgeted (`MAX_BLAS_COMPACTIONS_PER_FRAME = 8`) so concurrent old+new
  allocations stay bounded; sub-break-even sizes are skipped.
  `GODOT_RT_DUMP_COMPACTION=1` logs swap counts and bytes saved;
  `GODOT_RT_BLAS_COMPACTION=0` disables the lane. Deformed,
  merged-multimesh, and procedural BLASes are out of scope (updatable or
  rebuilt on mutation). Measured interaction: a compacted BVH legitimately
  resolves exact-tie hits differently on shared edges (E1 instance-ID parity
  showed max_difference=1 on 307/4096 pixels), so the intersector-parity test
  stage pins compaction off to keep isolating the traversal-lane variable;
  scene-stage image coverage keeps compaction enabled.
- **P3 — foundation implemented; alpha/procedural lanes pending.** The
  regex-style rewrite was replaced by an explicit traversal-class registry
  (`TRAVERSAL_CLASS_REGISTRY` in `metal_rt_shader_lowering.cpp`): each class
  declares its RT_FLAGS-derived guard constant (per-class pipeline
  specialization via function-constant folding), its injection anchors, its
  exclusions, and — for unimplemented classes — the requirements that block
  them. `apply_traversal_lowering()` is the metadata-driven engine;
  `patch_scene_ray_query_to_intersector()` survives as a compatibility facade
  with the original status vocabulary (existing fixtures unchanged). The
  shader container records `rt_traversal_applied_mask` /
  `rt_traversal_eligible_mask` per stage (container FORMAT_VERSION 3), and
  cache validation now compares that metadata against the active lane instead
  of re-parsing MSL. `alpha_triangles` and `procedural` are declared with
  their pending requirements (intersection-function tables + linked-function
  compute pipelines); implementing those lanes is the follow-on project this
  registry exists to host.
- **P4 — implemented (validation + gated enablement).** `blas_create` /
  `tlas_create` now validate primitive, geometry, and instance counts against
  Metal's standard limits (2^28 primitives, 2^24 geometries/instances,
  constants on `MDAccelerationStructure`). A structure that exceeds a limit is
  built with `MTLAccelerationStructureUsageExtendedLimits` (macOS 12+; older
  OS fails with an explicit error) and logs `METAL_RT_EXTENDED_LIMITS`.
  Coherence gate: because neither SPIRV-Cross's `intersection_query` emission
  nor the intersector lowering declares the `extended_limits` MSL tag yet,
  `prepare_tlas_build` refuses to build a TLAS over any extended-limits
  structure with an explicit once-warning instead of tracing undefined
  memory; visibility masks are 8-bit end to end, within the standard limit.
  AS build-batching profiling remains open; it is unblocked now that
  compaction exists but needs dedicated measurement runs.

## Implementation status (2026-07-17)

- **B1 — implemented.** `material_table_all_opaque` moved onto
  `RTViewportState` and is refreshed by `finalize_buffers()` inside every
  `build_tlas()`. `RenderForwardClustered` now derives `RT_FLAG_ALL_OPAQUE`
  *after* `build_tlas()` returns, from the same viewport's same-frame table,
  before `update_uniform_set()` and the trace dispatch. The cross-viewport
  global aggregate and the one-frame-stale window are gone. Consistency note:
  the HG-readiness checks inside `build_tlas()` run against the base-flags
  bundle; whenever the final flags add `ALL_OPAQUE`, the table contains no
  custom dispatches or alpha materials, so readiness against the base bundle
  and the opaque bundle agree, and `ensure_pipeline_bundle()` builds compute
  bundles synchronously at dispatch time.
- **B3 (Apple9 gate) — implemented.** The intersector MSL patch now defaults
  on only when `device_profile->gpu >= Apple9` (capability-based, no
  device-name parsing). `GODOT_MTL_RT_INTERSECTOR=1` forces the lane on for
  pre-Apple9 A/B runs; `=0` forces the query path everywhere.
- **B3 (macOS 15.2-15.3 refit fallback) — implemented.**
  `command_update_blas()` substitutes a full build for the refit on macOS
  15.2.x-15.3.x, matching Blender's workaround. Scratch allocations already
  cover both paths because `required_scratch_size()` takes the build/refit
  maximum for updatable structures.
- **Fast-trace usage mapping — implemented.**
  `MDAccelerationStructure::usage_from_flags()` maps `PREFER_FAST_TRACE` to
  `MTL::AccelerationStructureUsagePreferFastIntersection` behind
  `__builtin_available(macOS 26, iOS 26, tvOS 26)`, only when the AS carries
  neither `ALLOW_UPDATE` nor `PREFER_FAST_BUILD`. `LOW_MEMORY` intentionally
  stays unmapped.
- **B2 — implemented.** The production rewrite is now
  `MetalRTShaderLowering::patch_scene_ray_query_to_intersector()`, a
  transactional helper with stable status codes and tracked MSL fixtures for
  successful closest-hit/shadow injection, brace/anchor drift, missing
  `RT_FLAGS`, and procedural exclusion. The GPU suite patches and specializes
  one MSL library as both query and `ALL_OPAQUE` intersector, comparing hit,
  shadow, miss, `t_min`, and `t_max`. The dedicated E1/E2 stage adds
  intersector-versus-query image parity under Shader Validation for transforms,
  negative-scale/culling, double-sided, alpha, and mutation. Cache validation
  rejects MSL compiled for the other traversal lane. The E2 material-ID debug
  target is excluded because its existing same-lane cold/reload capture is
  nondeterministic. The existing
  M5 Metal System Trace capture records 93.9% occupancy; headless RT counter
  profiles are documented unavailable on this device/toolchain, so they are
  retained as reviewed external evidence rather than a false automated gate.

**Status (original):** Planned audit follow-up  
**Baseline:** Godot `6a823bbfe6` (`Metal RT: traverse opaque scenes with the native intersector`) compared with the local Blender MetalRT backend, including Apple-authored Cycles changes.

## Conclusion

The new opaque-triangle `intersector` lane is directionally correct and should
remain. It follows Apple's recommendation to prefer `intersector` over
`intersection_query` on Apple-family-9 hardware, where query traversal adds
scratch traffic and prevents ray reordering. The external current-transform
table, dedicated boolean shadow helper, `accept_any_intersection(true)` for
shadows only, and static-BLAS/no-refit policy are all consistent with Blender's
Apple-maintained implementation.

Do not request merge until the correctness and regression-coverage items below
are resolved. The larger compilation and acceleration-structure projects should
be tracked separately unless their performance evidence is required for this
merge.

Primary references:

- [Apple: Ray tracing with M3 and A17 Pro](https://developer.apple.com/videos/play/tech-talks/111375/)
- [Apple: Prefer fast intersection](https://developer.apple.com/documentation/metal/mtlaccelerationstructureusage/preferfastintersection)
- [Apple: Extended limits](https://developer.apple.com/documentation/metal/mtlaccelerationstructureusage/extendedlimits)

## Merge blockers

### B1 — Calculate opaque eligibility from current, per-viewport state

`RT_FLAG_ALL_OPAQUE` is selected before the current viewport material table is
built, from a global aggregate left by the preceding frame/viewport. That flag
skips alpha evaluation in GLSL and the new MSL lane force-opacifies traversal.
Consequently a newly added alpha-scissor material can render opaque for one
frame; two persistent viewports with different material sets can make the
alpha viewport take the wrong lane repeatedly.

Implement one of these safe policies:

1. Preferred: store the aggregate on `RTViewportState`, build/classify the
   current viewport table before deriving `rt_flags`, then select the matching
   pipeline.
2. Interim-safe: clear `ALL_OPAQUE` whenever the current material set or
   generation is dirty; use the general query lane until classification is
   complete.

Do not retain a cross-viewport global eligibility bit.

Evidence:

- Flag selection: `render_forward_clustered.cpp:2262-2267`
- Existing one-frame limitation: `render_raytracing.h:480-486`
- Alpha skip/opaque flag: `scene_raytracing_compute.glsl:279-321`
- Native helper: `rendering_shader_container_metal.cpp:401-440`

Required tests:

- opaque-to-alpha and alpha-to-opaque transitions;
- two viewports with stable, disjoint opaque/alpha material tables;
- alpha-cutout closest-hit and shadow parity against
  `GODOT_MTL_RT_INTERSECTOR=0`.

### B2 — Test and diagnose the production MSL rewrite

The HEAD change is a bounded textual rewrite of SPIRV-Cross output. Existing
GPU coverage validates a handwritten intersector kernel, not this emitted MSL
path. Make the rewrite a testable lowering helper and add fixtures for:

- successful closest-hit and shadow injection;
- expected query fallback when anchors drift;
- procedural/AABB exclusion;
- alpha, double-sided, culling, transform, `t_min`, and `t_max` semantics.

Log a structured reason when injection is intentionally skipped or an expected
anchor is absent. Keep the query fallback, but do not let an output-format
change silently erase the optimization.

Add GPU image/pixel parity runs for the default and forced-query lanes, and use
Shader Validation plus RT scratch/occupancy counters to prove the intended
hardware path rather than relying on image correctness alone.

### B3 — Apply Apple-family-9 policy and known OS reliability fallback

Use `supportsFamily(Apple9)` as the default eligibility gate for the native
`intersector` performance lane; retain the environment override for A/B and
diagnostics. Do not use Blender's device-name parsing. Benchmark M1/M2 before
claiming a benefit outside the hardware-RT tier.

For BLAS updates on macOS 15.2–15.3, substitute a build for refit. Blender's
Apple-maintained backend carries this workaround for missing geometry on those
OS releases.

## Small, direct policy improvement

Godot already sets `ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT` for static
BLASes and TLASes, but `MDAccelerationStructure::usage_from_flags()` ignores
it. On macOS 26+, map that request to
`MTL::AccelerationStructureUsagePreferFastIntersection` only when the AS is
not refittable and does not prefer fast build. This mirrors Blender's static
versus dynamic policy and makes the existing renderer intent effective without
altering old-OS behavior. Add a focused mapping/availability unit test.

`LOW_MEMORY -> MinimizeMemory` is available too, but must remain an explicit
quality/performance tradeoff rather than a default.

## Follow-up performance and scalability work

### P1 — Batch and asynchronously specialize compute material variants

The Metal compute route synchronously builds a growing generated shader for
each newly ready material slot. A burst of N new materials produces N complete
compiles and approximately O(N²) generated-source processing. Adapt Blender's
policy rather than its exact architecture:

- coalesce/debounce material changes;
- compile one all-active aggregate variant asynchronously;
- retain a generic or last-known-good pipeline while it compiles;
- atomically swap only generation-matching output at a frame boundary;
- bisect to isolate a failing material only after the aggregate fails;
- cache by aggregate source/features key.

Keep the initial HG0 bootstrap synchronous if necessary.

### P2 — Compact immutable BLASes safely

Godot records a compacted size but has no `copyAndCompact`/allocation swap.
Blender does the full lifecycle. Implement this only as an asynchronous,
graph-visible operation for immutable BLASes:

1. build and query compacted size;
2. wait/read size, allocate compact destination, then copy/compact;
3. retain the source AS until no submitted command can reference it;
4. atomically replace the cached AS and invalidate dependent TLASes;
5. bound concurrent old + new + scratch allocations.

Use the explicit 64-bit compacted-size API (or a 32-bit buffer with the legacy
API), not an ambiguous 8-byte legacy result buffer.

### P3 — Broaden native intersector coverage only through a real lowering design

Blender uses separate intersection-function tables for alpha/custom/procedural
work. In Godot, any such material sends the entire variant back to
`intersection_query`. The next traversal-performance project is a native
intersector/IFT path for alpha triangles, then procedural geometry. Do not grow
the current regex-style rewrite into that system; introduce explicit lowering
metadata and per-class pipeline specialization first.

### P4 — Validate limits and profile build batching

Add explicit scene-limit checks and enable `ExtendedLimits` only when a scene
exceeds standard Metal limits; the MSL intersector type and visibility-mask
handling must change coherently. Profile AS build batching after compaction or
concurrent build work exists—Blender's working-set throttle protects its async
build model, while Godot currently serializes builds.

## Evidence to attach to any merge request

- query-versus-intersector image/pixel parity matrix, including alpha/culling,
  transformed instances, closest hit, and shadows;
- Apple9 and pre-Apple9 capability/benchmark results;
- shader-validation and RT-counter captures showing the fast lane's scratch and
  occupancy behavior;
- macOS 15.2–15.3 refit fallback result;
- generated-MSL fixture results, including intentional fallback diagnostics.
