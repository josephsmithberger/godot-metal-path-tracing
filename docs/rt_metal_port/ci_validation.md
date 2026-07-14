# Metal ray tracing CI and image validation (C12)

Status: **Available**. The workflow is committed and locally repeatable; the
self-hosted job is intentionally dormant until the repository has a runner
with the required capability label.

Chunk C12 turns the C2-C11 probes and smokes into one retained validation
record. Hosted macOS remains responsible for compile, boot, and unit health.
A separately labeled Apple Silicon runner is responsible for native GPU
execution, the reviewed image regression, and runtime fallback behavior.

## CI lanes

`.github/workflows/macos_builds.yml` now has two complementary Metal jobs:

| Job | Runner | Contract |
|---|---|---|
| `build-macos-metal-only` | GitHub-hosted `macos-26` | Build the arm64 Metal-only editor, run boot/unit checks, and publish the binary and logs. |
| `test-metal-rt` | `[self-hosted, macOS, ARM64, metal-rt]` | Download that exact binary, record capabilities, run C5-C10 GPU tests with Metal validation, compare the C10 image, run both C11 gates, and always upload the evidence. |

The GPU job has a 30-minute timeout and a global
`metal-rt-apple-silicon` concurrency group so two workflow runs cannot drive
the same lab GPU concurrently. It runs only when either:

- repository variable `ENABLE_METAL_RT_CI` is exactly `true`; or
- a manual macOS workflow dispatch enables `run-metal-rt`.

Before setting the repository variable, register an Apple Silicon runner with
the custom `metal-rt` label. GitHub's `macOS` and `ARM64` labels plus the
custom label are the scheduling capability check; the first step also rejects
an unexpected runner architecture. The runner needs Xcode command-line tools
and Python 3. It does not rebuild Godot or install dependencies.

## Metal shader validation

Every stage that dispatches real ray-tracing work runs with `MTL_DEBUG_LAYER`
and `MTL_SHADER_VALIDATION` enabled, and fails if the log contains
`Invalid device load` or `Invalid device store`.

This exists because the RT compute lane reads geometry and material data
through raw device addresses. The API debug layer cannot see those accesses,
and an invalid one does not raise the process exit code: the kernel reads
garbage, still produces an image, and still prints every acceptance marker. A
run can therefore be green on every marker and every capture check while the
GPU is faulting on each frame — this is exactly how the C15 residency defect
reached a commit. Shader validation instruments the shader itself, so it is
the only gate in this lane that observes those reads.

Enabling validation is not free: it slows the editor capture stages and it
zeroes invalid accesses rather than returning whatever the address happened to
hold. That second property is a feature here — it makes a latent address bug
render as an obvious black or missing surface rather than as plausible noise.

## Image regression

The reviewed reference and its policy live together:

- `tests/metal_rt/references/c10_pathtracer_launch_v1.png`;
- `tests/metal_rt/references/c10_pathtracer_launch_v1.json`.

The manifest pins the source commit, device/OS, fixed C10 scene and camera
revision, 8x8 resolution, two samples per pixel, bounce count, direct-RGBA8
color encoding, reviewer/date, and reference SHA-256. The comparison accepts a
maximum absolute channel difference of 5/255 (below C10's existing 0.02
floating-point bound). The current Apple M5 output is byte-for-byte identical.

`tests/metal_rt/image_diff.py` uses only the Python standard library. It
validates the manifest and reference hash, decodes non-interlaced RGBA8 PNGs,
compares every channel, and writes these artifacts even for a numeric
mismatch:

- `c10_pathtracer_launch_diff.png`, with RGB differences amplified 32x;
- `c10_pathtracer_launch_metrics.json`, including the reference identifier,
  image hashes, max/mean/RMS difference, differing channel/pixel counts, and
  the pass threshold.

An exact match produces an opaque black diff. A changed region appears in its
corresponding RGB channel; any max channel difference above 5 fails the stage.
Manifest, decode, dimension, or reference-integrity errors are distinct hard
failures rather than skips.

The comparer also has two opt-in exact modes:

- `--exact-pixels` requires identical decoded RGBA8 values while allowing PNG
  metadata, compression, and chunk-layout differences;
- `--exact-bytes` additionally requires the complete encoded files to be
  byte-for-byte identical.

Exact pixels are appropriate for deterministic integer/debug outputs. Exact
bytes are useful for a fixed encoder/toolchain artifact check, but are not a
cross-GPU path-tracing gate: PNG encoding may differ even when every pixel is
identical, and floating-point GPU results may require the reviewed tolerance.
`python3 tests/metal_rt/test_image_diff.py` exercises threshold, exact-pixel,
and exact-byte behavior and is included in the runner's `preflight` stage.

## Running locally

Run the complete C12 GPU contract against an existing Metal-only editor:

```bash
MTL_DEBUG_LAYER=1 python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage gpu \
  --stage image \
  --stage fallback \
  --binary bin/godot.macos.editor.arm64 \
  --fail-on-skip \
  --keep-going
```

Run only the focused render and image comparison:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage image \
  --binary bin/godot.macos.editor.arm64
```

The `image` stage runs the C10 GPU case itself unless `gpu` was also selected,
in which case it reuses that stage's image. The runner always probes
capabilities first and records an allowed machine-readable skip on unsupported
hardware.

## Artifact and pass contract

The self-hosted job uploads `macos-metal-rt-validation` unconditionally. Its
directory contains:

- `summary.json` with commit, dirty state, host, exact commands, statuses,
  durations, and the artifact inventory;
- one log per command;
- `capability_record.json`;
- GPU and CPU-reference PNGs emitted by C10;
- the reviewed-reference diff and comparison metrics.

The job passes only when the capability probe is consistent, all C5-C10 GPU
cases pass without a skip, the image stays within tolerance, the supported
C11 launch reports `C11_GATE=enabled`, and the forced-disable launch reports
the non-RT fallback plus `C11_GATE=disabled:forced_disabled`. Artifact upload
uses `if: always()` so a failing job retains enough evidence to diagnose it.
