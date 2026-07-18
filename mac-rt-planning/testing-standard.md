# Metal ray tracing testing standard

## Purpose

This standard defines what evidence is required while implementing native
Metal ray tracing. It supplements Godot's existing test conventions; it does
not replace the engine-wide contributor or unit-testing guidance.

The core rule is that every porting chunk must end in a repeatable test with a
clear pass condition and retained diagnostics. A successful compile is useful
evidence, but it is not evidence that ray tracing works.

## Test layers

| Layer | Required environment | Purpose | Current state |
|---|---|---|---|
| L0 Static | Any development machine | Formatting, generated-file, and focused source checks | Available through existing repository tools |
| L1 Build | macOS toolchain; x86_64 or arm64 as appropriate | Catch compile/link and platform-gating regressions | Available |
| L2 Engine smoke | Runnable macOS editor binary | Prove the binary starts and exposes the expected CLI | Available |
| L3 Unit | Editor built with `tests=yes` or `dev_mode=yes` | Validate CPU-side layouts, handle lifetimes, descriptor conversion, and validation logic | Available, including C4-C9 Metal RT cases |
| L4 GPU functional | Supported Apple Silicon Metal device | Exercise BLAS, TLAS, pipeline setup, dispatch, synchronization, and fallback | BLAS build/refit available in C5, one-instance TLAS build available in C6, hit/miss trace kernels (SPIRV-Cross ray query + native intersector) available in C7, a native pipeline/table plus exact-hash RGBA8 trace image available in C8, mapped Godot group/function-table resources available in C9, the controlled path-tracer launch available in C10, and supported/forced-fallback runtime gates available in C11 |
| L5 Image regression | Supported Apple Silicon device with a fixed scene | Detect visible correctness regressions with reference and diff images | L5-C is Available for the C10 controlled scene through C12; L5-E editor profiles are Planned |
| L6 Stability/performance | Named lab machines | Catch lifetime, synchronization, memory, and severe performance regressions | Planned |

Tests should be placed at the lowest layer that can prove the behavior. For
example, instance descriptor packing belongs in L3; successful construction of
an `MTLAccelerationStructure` belongs in L4.

L5 has two scopes. **L5-C** is the controlled C10 compute-scene regression.
**L5-E0** through **L5-E3** are real editor-scene profiles for HG0, geometry,
materials, and procedural content. Their fixtures and capture rules are defined
in [`editor-scene-testing.md`](editor-scene-testing.md). Passing L5-C does not
imply that a scene can be viewed in the editor.

## Required change gates

Use the following minimum gates for each kind of change. A pull request may need
additional coverage when it crosses more than one category.

| Change type | Required before merge |
|---|---|
| Documentation or test-runner-only | Runner self-check/dry run and documentation link check |
| Shared rendering interface | L1 on arm64 and x86_64, L2, full L3 |
| Metal backend without GPU execution | L1 arm64 Metal-only, L2, focused L3, full L3 |
| BLAS/TLAS allocation or build | Previous gates plus focused L4 and repeated resource teardown |
| Pipeline/function-table/dispatch | Previous gates plus deterministic one-ray L4 case |
| Path tracer integration | Previous gates plus L5 image comparison and fallback test |
| Editor scene routing or content | Previous gates plus the matching L5-E profile, editor reload, and fallback test |
| Synchronization or resource lifetime | Relevant L4/L5 test repeated at least 20 times; no crash, device error, or leak diagnostic |
| Performance-sensitive path | Correctness gates plus L6 before/after measurements on the same named machine |

During early bring-up, a GPU test may be marked experimental, but it must not be
described as a required merge gate until a runner with the documented hardware
is continuously available.

## Naming and placement

- C++ unit-test names should follow existing Godot conventions and include
  `[MetalRT]`, for example `TEST_CASE("[MetalRT] Packs triangle geometry")`.
- Focused GPU test names should be stable, backend-specific, and describe one
  behavior: `blas_triangle_build`, `tlas_single_instance`, or
  `trace_one_triangle`.
- Test scenes and scripts should eventually live under `tests/metal_rt/`, not in
  the planning directory. Reference images should live beside a manifest that
  records how they were produced.
- Artifact directories use `<UTC timestamp>-<arch>-<stage>` when created by the
  runner. CI may prepend a job or run identifier.
- Every artifact summary must record the source commit, dirty-tree state,
  machine architecture, macOS version, selected stages, exact commands, and
  exit codes.

## Pass, skip, and failure contracts

A test passes only when its process exits with zero and all layer-specific
assertions pass. Absence of a crash is not sufficient for GPU or image tests.

A skip is acceptable only when the test emits a machine-readable reason from
this list:

- `unsupported_os`
- `unsupported_arch`
- `unsupported_gpu_family`
- `missing_metal_rt_feature`
- `missing_test_asset`
- `runner_not_configured`

Unexpected capability detection, shader compilation failure, Metal validation
errors, command-buffer errors, missing output, and image mismatch are failures,
not skips. A forced-disable fallback test is expected to run on supported
hardware and must not be skipped merely because RT was disabled by the test.

Flaky tests are treated as defects. A failing required test may be quarantined
only with an owner, a tracking issue, a narrow scope, and an expiry date. Retry
results may help diagnosis but do not turn an initial failure into a pass.

## GPU functional contract

Each L4 run must write a capability record before exercising the GPU. At a
minimum it should contain:

- device and registry name;
- macOS version and process architecture;
- supported Metal GPU families used by feature gating;
- argument-buffer tier and Metal language version;
- native ray tracing availability and every optional capability selected by the
  implementation;
- whether Metal API validation was enabled.

The initial deterministic scene should contain one triangle, one BLAS, one TLAS
instance, one miss path, and one closest-hit path. Its output should encode
values that can distinguish hit from miss; a uniform "completed" image is not
enough. The test passes only if command buffers complete without error and the
expected pixels or output-buffer values match.

Resource-lifetime coverage must include create/build/use/destroy, rebuild, and
process shutdown. Refit and compaction require separate cases when implemented.

## Image regression policy

Reference images are reviewed test data, not screenshots copied from a local
run. Each reference manifest must record:

- scene and camera revision;
- engine commit that generated the candidate;
- device family, OS, resolution, renderer, and relevant quality settings;
- comparison metric and threshold;
- reviewer and approval date.

Comparison should happen in a declared color space. The harness must save the
actual image, reference image identifier, visual diff, and numeric metrics. Use
exact comparison only for deliberately integer/deterministic output. Path
tracing tests must use a fixed seed and sample count, with a justified tolerance
for cross-family floating-point differences.

The canonical comparer supports three explicit policies:

- threshold mode (the manifest default) for path-traced beauty images;
- `--exact-pixels` for deterministic decoded RGBA8 outputs such as IDs/masks;
- `--exact-bytes` only for deterministic encoder/artifact checks on the same
  toolchain.

Byte-for-byte PNG equality is not a cross-GPU correctness gate because metadata
and compression can change without changing pixels. The commands and self-test
are documented in [`editor-scene-testing.md`](editor-scene-testing.md).

Never update a golden image in the same step that silently accepts it. Generate
a candidate, inspect its diff, and approve the reference as a separate review
decision.

## Performance and stability

Performance measurements are advisory until correctness is stable. Record BLAS
build, TLAS build/update, dispatch, and total frame timing separately. Compare
only runs from the same machine, power configuration, scene, build type, and
sample count. A performance result without those fields is not actionable.

Stability runs should use Metal API validation in a debug/dev build and repeat
the smallest failing case. Save stderr and command-buffer error details. Longer
soak tests should also record peak memory and the iteration at which a failure
occurred.

## Pull-request evidence checklist

Every RT port pull request should state:

- the C1-C18 chunk and test layers affected;
- exact commands run;
- runner hardware and macOS version for GPU results;
- links or paths to summaries, logs, capability records, and image diffs;
- skipped required lanes and why;
- known unsupported behavior and its fallback.

The standard local entry point and current limitations are documented in
[`automation.md`](automation.md).
