# Test automation

## Local runner

Use `tests/metal_rt/run_mac_rt_tests.py` as the shared entry point for local macOS
Metal builds and the engine tests that exist in this branch.

The default `all` run performs:

1. host and toolchain preflight checks and image comparer self-tests;
2. the Metal capability probe (chunk C2): compiles
   `docs/rt_metal_port/capability_probe.mm` and writes
   `capability_record.json` into the artifact directory;
3. an arm64 editor build with `dev_mode=yes`, `metal=yes`, and optional backends
   disabled;
4. `--version` and `--help` smoke checks;
5. the existing C++ unit suite with `--test --force-colors`.
6. the C5 single-triangle BLAS build/refit, C6 one-instance TLAS, C7 shader-lane
   hit/miss, C8 exact-hash trace-image, and C9 mapped pipeline/function-table
   GPU smokes, plus the C10 controlled path-tracer launch, on supported Apple
   Silicon hardware;
7. the C11 supported runtime gate and forced-disable non-RT fallback launches;
8. the C12 reviewed image comparison, visual diff, and numeric metrics.

Run only the capability probe:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py --stage caps
```

Validate the future chunk package without building or launching Godot:

```bash
python3 mac-rt-planning/scripts/check_next_chunks.py \
  --json-output mac-rt-planning/artifacts/plan-check.json
```

Run just the comparer unit tests:

```bash
python3 tests/metal_rt/test_image_diff.py
```

Run the C5-C10 GPU smokes against an existing binary:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py \
  --stage gpu \
  --binary bin/godot.macos.editor.arm64
```

The `gpu` stage runs the capability probe first. It records
`missing_metal_rt_feature` when the device cannot execute the test and
`unsupported_arch` for non-arm64 requests. A test process that exits zero but
declares `SKIP_REASON=<reason>` in its output is recorded as skipped, not
passed, per the testing standard's skip contract.

Run the C11 runtime/fallback pair against an existing binary:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py \
  --stage fallback \
  --binary bin/godot.macos.editor.arm64
```

The stage runs the capability probe, launches `tests/metal_rt/` through Metal
and requires `C11_GATE=enabled`, then repeats with
`GODOT_MTL_DISABLE_RAYTRACING=1` and requires the explanatory non-RT fallback
marker. A zero process exit without the expected marker is a failure.

The probe exits with a machine-readable skip (recorded as `skipped`, not
`failed`) when no Metal device is available, and fails when a measured
capability contradicts the family expectations in
`docs/rt_metal_port/capability_matrix.json`.

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py
```

Preview the commands and output location without executing anything:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py --dry-run
```

Run selected stages against an existing binary:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py \
  --stage smoke --stage unit \
  --binary bin/godot.macos.editor.arm64
```

Append SCons options after the standardized defaults:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py \
  --stage build --scons-flag compiledb=yes --jobs 8
```

Use `--keep-going` when collecting diagnostics from independent stages. The
summary still exits nonzero if any command failed. Without it, the runner stops
at the first failure.

## Output contract

Each run creates one directory containing:

- `summary.json`: host, Git state, stages, exact commands, durations, and exit
  codes;
- one numbered `.log` file for every executed command.

The default root is `mac-rt-planning/artifacts/`. Override it with
`--artifact-dir`. The directory is ignored by Git so local evidence is not
accidentally committed.

The runner deliberately does not install dependencies. A missing `scons`,
`xcrun`, SDK, or binary is a preflight/build failure with a retained diagnostic.
Dependency installation should remain explicit in local setup and CI.

## Scope and limitations

The runner automates L1-L3 from `testing-standard.md`, plus the capability
record that the GPU functional contract requires before any L4 run (the `caps`
stage). Chunks C5-C10 add narrow L4 claims for native BLAS allocation, build,
compacted-size query, refit, one-instance TLAS build, instance parameter
round-tripping, shader lowering, native intersector dispatch, pipeline-specific
intersection-function-table setup, exact-hash RGBA8 output, and Godot
shader-group/SBT resource mapping, plus a controlled path-tracer compute
dispatch compared with a CPU reference. C11 adds the capability-gated launch
and forced non-RT fallback checks. C12 adds the reviewed L5 golden image,
manifest validation, diff artifact, and numeric comparison metrics.

The `image` stage is therefore Available for L5-C only. It does not launch an
editor fixture and must not be used as editor-integration evidence. When the
C13-C18 runtime fixtures land, extend this runner with `editor-scene`, `export`,
and `stability` stages rather than adding unrelated one-off shell commands.
Those stages must honor the artifact and skip contracts in the testing
standard.

## Image comparer automation

`tests/metal_rt/image_diff.py` is the only canonical PNG comparer. Its default
threshold mode is used by C12. `--exact-pixels` overrides the manifest threshold
with decoded RGBA8 equality; `--exact-bytes` additionally requires complete PNG
file identity. The latter is useful for deterministic encoder/artifact checks,
not cross-GPU path-traced beauty images.

The self-test constructs PNGs with identical pixels but different metadata to
prove that exact-pixel and exact-byte modes stay distinct. Keep this test in the
preflight stage when the comparer changes.

## Next-chunk plan automation

`mac-rt-planning/scripts/check_next_chunks.py` validates the C13-C18 JSON index,
ordered IDs, dependencies, status vocabulary, test profiles, acceptance
markers, synchronized per-chunk Markdown status, and local Markdown links. It
can write a JSON result for CI or review artifacts. This check validates the
plan only; it never marks a runtime feature Available.

## CI adoption

Step 1 landed as chunk C3: the `build-macos-metal-only` job in
`.github/workflows/macos_builds.yml` builds the arm64 editor with
`metal=yes vulkan=no angle=no accesskit=no`, runs the smoke/unit checks, and
uploads binary and log artifacts. It reuses the repository's existing
`godot-build` composite action rather than invoking this Python runner, so no
binary is built twice; the runner remains the local mirror of the same flag
set. The lane contract is documented in `docs/rt_metal_port/build_lane.md`.

Step 2 landed as chunk C12: `test-metal-rt` is a separately labelled,
opt-in self-hosted Apple Silicon job with concurrency control, a timeout,
strict no-skip behavior, and unconditional artifact upload. It stays dormant
until `ENABLE_METAL_RT_CI=true` and a `[self-hosted, macOS, ARM64, metal-rt]`
runner is registered.

The hosted job remains responsible for general macOS compile health. The
self-hosted job is responsible for native Metal RT behavior. A universal binary
running on a hosted Mac is not a substitute for recording the actual GPU family
and exercising the RT path.

A CI job should upload `summary.json` and all logs even after failure. GPU jobs
must additionally upload the capability record, actual output, reference
identifier, diff image, and comparison metrics.
