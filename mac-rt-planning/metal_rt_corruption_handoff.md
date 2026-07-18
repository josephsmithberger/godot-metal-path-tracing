# Metal RT path-tracing corruption: investigation handoff

Investigation status: **RESOLVED 2026-07-16 (workaround shipped; Apple compiler
bug still unreported upstream).** The corruption trigger was confirmed to be
the shadow-ray `intersection_query` traversal being inlined into the
path-tracing mega-kernel. The fix keeps *only* `trace_shadow_blocked` behind a
`noinline` boundary in the generated MSL and re-declares its query object as a
function-local (instead of the SPIRV-Cross-hoisted by-reference parameter that
lives in `main`'s frame). This preserves correctness at full-inline speed:

| generated-MSL mode (`GODOT_MTL_RT_NOINLINE`) | raw zero-channel pixels (e3_debug, no validation) | pathtracer @1080p 1spp/2bounce |
|---|---|---|
| `lights` (old safe default: noinline `lights_evaluate_direct_lighting`) | ~100-131 (scene baseline) | 11.0-11.3 ms |
| `shadow` (NEW DEFAULT: noinline `trace_shadow_blocked` + local query) | ~110-135 (same baseline) | **5.8-6.1 ms** |
| `none` (full inline) | ~450,000-485,000 (corrupt) | 5.7-5.8 ms |

Measured with the raw RGBA16F readback metric (`GODOT_DBG_DUMP_RT=1`, now a
tracked hook in `render_raytracing.cpp:copy_output_texture`), no shader
validation, camera moving and static, off-screen perf harness with 60 s
cooldowns between runs (see "thermal contamination" below). Escape hatches:
`GODOT_MTL_RT_NOINLINE=lights|shadow|none` selects the boundary;
`GODOT_MTL_RT_SHADOW_REF=1` keeps the hoisted by-reference query in shadow
mode.

Perf-measurement trap found while validating: back-to-back GPU-saturated runs
thermally contaminate each other (an 11 ms config degraded to 18 ms over one
70 s run and biased the *next* run's first windows). Compare modes only with
short runs, alternating order, idle cooldowns, and min/median-of-windows.

The historical investigation notes below are retained for context.

## Mission

Fix the original RT path-tracing corruption regression following performance
tuning in the editor lane. The reported repro is:

```sh
./bin/godot.macos.editor.arm64 --editor --path tests/metal_rt/editor \
  res://fixtures/e3_procedural.tscn
```

The symptom is sparse, colored/black pixel speckling in miss/background pixels.
It flickers when the camera moves, freezes when the camera is still, and moves
after toggling path tracing. It had improved during investigation, but the user
now reports the original artifact again after performance tuning. Treat the
performance changes as the immediate regression range to bisect.

Current branch/commit: `nvidia-pt-dlss` at `8122cb4408`.

## Important correction to the initial diagnosis

An earlier agent concluded this was an inter-pass synchronization race: the
editor copied the RT output while its compute dispatch was incomplete. That
explanation was plausible and produced two valid Metal-driver fixes, but it
does **not** resolve the user's repro.

The later raw-texture readback disproved the key premise: the RT output texture
already contains corrupt pixels before `copy_output_texture()` runs. Treat the
copy/presentation theory as a secondary hardening concern, not the current root
cause.

## Current strongest hypothesis for the original speckling

This looks like a Metal shader-compiler / GPU code-generation problem involving
two `metal::raytracing::intersection_query` objects in the generated compute
kernel. SPIRV-Cross lowers one `rayQueryEXT` declared in `trace_material()` and
one in `trace_shadow_blocked()` to separate query objects hoisted into `main`.
Their semantic lifetimes do not overlap, but the generated MSL makes both large
objects live across the kernel.

The current uncommitted GLSL change replaces them with a single shared
`rayQueryEXT rt_query` near
`servers/rendering/renderer_rd/shaders/raytracing/scene_raytracing_compute.glsl:280`.
The worktree also changes the generated MSL so
`lights_evaluate_direct_lighting` is not force-inlined when an intersection
query is present. Both are experiments aimed at query lifetime/register
pressure. Neither has been verified as a complete fix across the production
scene/material path or the ordinary Vulkan/SPIR-V path. Inspect the resulting
MSL to confirm the intended query count and call boundary rather than inferring
them from the GLSL/source rewrite.

## Observations and discriminating experiments

These are observed outcomes from the Claude session, in chronological order.

1. The original raw RT vs presented-copy conclusion was reversed. With the
   `GODOT_DBG_DUMP_RT` hook, the raw `RGBAH` path-tracer output at frame 300
   contained large numbers of pixels with one color channel exactly zero
   (`zero_gb=134816` in one run), with no non-finite values and no negative
   values. The RT dispatch, rather than the copy pass, writes bad data.
2. Argument-buffer inspection found the color output at binding 0 / argument
   texture slot 0 as an `RGBA16Float` Metal texture. Other sampled/output
   textures had distinct slots and expected formats. The generated MSL uses
   `texture2d<float, access::write>` for the output; it is not an invalid
   `read_write` RGBA16F texture. The only `image.write` is the final output
   store; the output is threaded into helper functions only because they use
   `imageSize()`.
3. A constant-environment experiment produced raw bad pixels such as
   `(0.0625, 0.125, 0.0, 1.0)` next to clean
   `(0.0625, 0.125, 0.25, 1.0)`. R/G/A were correct and only B was zero. This
   does not look like a whole-pixel or 4-byte aliasing overwrite.
4. Replacing the final stored value with a literal constant while keeping
   `total_radiance` live made the output clean (`zero_gb=0`). Therefore the
   storage image write is sound; a component of the computed radiance is already
   wrong.
5. Replacing `throughput * sample_environment(...)` with a direct literal add
   did not help (one run reported 119216 corrupted pixels). Thus the bad value
   can arise from `radiance += vec3(...)` after ray-query traversal, not the
   environment sample or throughput multiplication.
6. Generated MSL for the investigated loop was structurally correct and its
   `[[dont_unroll]]` GLSL annotations did not survive as equivalent loop hints.
7. `MTL_SHADER_VALIDATION=1` made the corruption disappear (`zero_gb=0`) and
   reported no invalid device loads/stores. This is especially significant:
   validation changes shader instrumentation/register allocation, so all current
   automated RT capture stages mask this issue.
8. Switching Metal math mode from fast to safe did not cure it; it merely
   changed the amount of corruption (reported 71840 bad pixels in one run).
9. Eliminating the shadow-ray path made the output clean. Restoring lighting
   math while returning from `trace_shadow_blocked()` before it constructs a
   ray query also remained clean. This isolates the trigger to two query
   objects, rather than overall shader size or the shadow-lighting math.

## Experiments that did not fix the user-visible artifact

### Driver synchronization/resource tracking

The following source changes remain uncommitted and should be reviewed,
retained only if independently correct, but they did not eliminate the problem.

- `drivers/metal/rendering_device_driver_metal.cpp`: maps RT shader-stage bits
  to compute resource usage so automatic Metal hazard tracking declares RT
  resources to the compute encoder.
- `drivers/metal/metal_objects_shared.h`: maps RT pipeline stages to
  `MTL::StageDispatch` and AS-build stages to `MTL::StageAccelerationStructure`
  for source/destination barriers.
- `GODOT_MTL_FORCE_BARRIERS=1` had previously been ineffective because the
  RT stage converted to zero. This mapping repair is still logically sensible,
  but it does not explain corruption already present in raw output.
- `GODOT_MTL_DBG_SYNC_SUBMIT=1` waits for each committed command buffer;
  `GODOT_MTL_DBG_NO_RING_RESET=1` stops scratch-ring reuse. These were added to
  distinguish in-flight CPU/GPU races. Do not treat a result under them as proof
  of the shader root cause without recording it.

### Data, binding, and math hypotheses

- RGBA16F access qualifier / bad `read_write` theory: rejected; generated MSL
  used `access::write`.
- Argument-buffer slot collision / wrong texture binding: rejected by the dump
  noted above, though verify with a fresh dump if source changes.
- Copy-to-viewport/presentation corruption: rejected for this artifact because
  `GODOT_DBG_DUMP_RT` catches it before the copy.
- Fast-math miscompile: not fixed by `GODOT_MTL_DBG_SAFE_MATH=1`.
- General shader size: not sufficient; preserving lighting while suppressing
  construction of the shadow query fixed the corruption.

## Current uncommitted worktree

Tracked files currently changed:

- `drivers/metal/metal_objects_shared.h`: RT/AS stage mappings.
- `drivers/metal/rendering_device_driver_metal.cpp`: RT compute resource-usage
  mapping.
- `drivers/metal/rendering_shader_container_metal.cpp`: generated-MSL
  `noinline` experiment for `lights_evaluate_direct_lighting`.
- `servers/rendering/renderer_rd/shaders/raytracing/scene_raytracing_compute.glsl`:
  current shared-query workaround candidate.

Untracked debug fixtures:

- `tests/metal_rt/editor/fixtures/e0_debug.tscn`
- `tests/metal_rt/editor/fixtures/e3_debug.tscn`
- `tests/metal_rt/editor/scripts/debug_presented_capture.gd`

The debug scene script captures full-resolution editor viewport frames while
the camera or a directional light moves, then after it becomes static. It is
enabled by `GODOT_DBG_CAPTURE_DIR`; optional knobs include
`GODOT_DBG_CAPTURE_MODE=camera|light`, `GODOT_DBG_SPP`,
`GODOT_DBG_BOUNCES`, and `GODOT_DBG_VIS`.

## Suggested next steps

1. Start from the exact user repro without `MTL_SHADER_VALIDATION`. Record the
   performance-tuning configuration and build identity, then cache-bust/rebuild
   before each shader experiment.
2. Capture both the raw `RGBAH` RT output and the presented viewport on a frame
   that contains the original artifact. Record per-channel values from corrupt
   and nearby clean pixels to reconfirm that the regression is written by the
   RT dispatch rather than introduced during presentation.
3. Bisect the performance-tuning changes against the last known clean
   configuration. Keep the non-instrumented raw-output metric enabled during
   the bisection; validation-enabled runs are not a substitute.
4. Use `GODOT_MTL_DBG_DUMP_MSL=<empty-output-dir>` and compare the generated
   production kernel before/after the one-query change. Count/locate
   `intersection_query` declarations and confirm the query object's reset,
   commit, and hit data are never used after the subsequent shadow trace.
5. Validate the shared-query GLSL both with the Metal lane and the ordinary
   Vulkan/SPIR-V path. If file-scope `rayQueryEXT` is invalid or gets lowered
   unexpectedly, use a structural alternative that causes one MSL query object
   while retaining per-invocation/private semantics (for example refactor both
   traversals to receive a single query object from a scope supported by the
   compiler).
6. Run the constant-environment discriminator before accepting the fix: raw
   output should have `zero_gb=0` without shader validation. Then restore the
   normal environment fetch and re-run E3 and E2. Check explicitly for the
   sparse channel-zero speckles.
7. Add a **non-instrumented** regression lane. The existing
   `tests/metal_rt/run_mac_rt_tests.py` deliberately exports
   `MTL_SHADER_VALIDATION=1` for all real RT stages, so its passing captures are
   not evidence against this compiler issue. A dedicated capture/metric stage
   using `e3_debug.tscn`, no validation, and a raw RT readback or
   full-resolution viewport metric is required.
8. Do not close visual acceptance until raw and presented captures are clean in
   static and moving-camera cases, at more than one view, with normal and
   constant-environment discriminators, and with validation both off and on.
9. Only after the raw texture is clean, decide which of the synchronization
   changes are production fixes versus temporary diagnostics. Remove the debug
   environment hooks and debug fixtures from any final minimal patch unless a
   separately reviewed test uses them.

## Useful commands/knobs

```sh
# User repro (no validation):
./bin/godot.macos.editor.arm64 --editor --path tests/metal_rt/editor \
  res://fixtures/e3_procedural.tscn

# Raw RT texture at frame 300 and generated MSL:
GODOT_DBG_DUMP_RT=/tmp/godot-rt-raw \
GODOT_MTL_DBG_DUMP_MSL=/tmp/godot-rt-msl \
./bin/godot.macos.editor.arm64 --editor --path tests/metal_rt/editor \
  res://fixtures/e3_debug.tscn

# Full-resolution presented-frame debug capture:
GODOT_DBG_CAPTURE_DIR=/tmp/godot-rt-presented \
./bin/godot.macos.editor.arm64 --editor --path tests/metal_rt/editor \
  res://fixtures/e3_debug.tscn
```

`tests/metal_rt/run_mac_rt_tests.py --stage procedural-scene` remains useful
for build/functional coverage, but its validation environment changes the
behavior under investigation.
