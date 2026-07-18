# Metal RT editor-scene testing contract

## Purpose

The existing C5-C12 tests prove backend operations and a controlled compute
scene. This contract defines the additional evidence required for real editor
integration. It extends [`testing-standard.md`](testing-standard.md); it does
not replace the narrow GPU tests.

## Editor profiles

| Profile | First required | Fixture purpose | Minimum output |
|---|---|---|---|
| L5-E0 HG0 | C13 | Opaque StandardMaterial3D, fixed camera/environment/light, hit and miss | Beauty, instance ID, metrics |
| L5-E1 geometry | C14 | Indexed/non-indexed, compressed, transformed, deformed, and MultiMesh content | Beauty, instance/primitive ID, rebuild counters |
| L5-E2 materials | C15 | Opaque, alpha cutout, double-sided, textured, and custom spatial material | Beauty, material ID, variant/cache record |
| L5-E3 procedural | C16 | Procedural AABB, triangle/procedural mix, bounds update | Beauty, hit kind/geometry ID, update record |

The fixture names and contents are versioned test data. Adding an unreviewed
object to a fixture is a test change, not ordinary scene cleanup.

## Proposed tracked layout

Create the runtime assets as their implementation chunks land:

```text
tests/metal_rt/editor/
  project.godot
  fixtures/
    e0_hg0.tscn
    e1_geometry.tscn
    e2_materials.tscn
    e3_procedural.tscn
  scripts/
    capture_editor_scene.gd
  references/
    <fixture>/<revision>/<image>.png
    <fixture>/<revision>/manifest.json
```

Do not add placeholder scenes that the current editor route cannot render;
that would make asset presence look like feature completion.

## Determinism contract

Every captured fixture must declare:

- scene and camera revision;
- renderer, resolution, output color space, and output format;
- fixed RNG seed, samples per pixel, bounce count, and frame accumulation rule;
- light/environment values and imported asset hashes;
- denoiser and presentation/upscaler mode;
- warm-up frames and exact frame at which capture occurs;
- engine commit and dirty-tree state;
- Mac model, Metal device name/family, architecture, macOS version, and Metal
  validation state.

The smoke resolution is 64x64 unless a chunk justifies another size. A larger
review reference may coexist, but CI should use the smallest image that still
distinguishes the behavior.

Camera cuts, project reloads, scene edits, and resizes must reset accumulation
at a defined point. A test that happens to capture an arbitrary accumulated
frame is invalid.

## Capture contract

Editor automation should open the fixture, wait for import and shader
compilation completion, assert the selected renderer and RT route, reset
accumulation, render the declared frames, save all required outputs, and exit.
The process must emit the chunk's acceptance marker only after image files and
metrics have been flushed.

A successful capture also requires:

- no Metal validation or command-buffer error;
- the expected capability and route markers;
- nonzero hit and miss counts;
- the expected scene revision and material/geometry counts;
- no unexpected skip;
- a clean shutdown.

A beauty image alone is insufficient because several routing failures can
produce a plausible black, rasterized, or stale frame.

## Image comparison modes

The canonical comparer is `tests/metal_rt/image_diff.py`. It validates the
reviewed reference manifest and always writes a visual diff and JSON metrics.

### Threshold mode (default)

Use for editor path tracing and cross-family GPU results:

```bash
python3 tests/metal_rt/image_diff.py actual.png reference.png \
  --manifest reference.json --diff diff.png --metrics metrics.json
```

The reviewed manifest owns the channel-difference threshold. Use a fixed seed
and justify any nonzero tolerance.

### Exact decoded pixels

Use for deterministic integer/debug outputs such as instance IDs, masks, and
the existing exact trace image:

```bash
python3 tests/metal_rt/image_diff.py actual.png reference.png \
  --manifest reference.json --diff diff.png --metrics metrics.json \
  --exact-pixels
```

This compares decoded RGBA8 values and ignores harmless PNG metadata,
compression level, and chunk-layout differences.

### Exact encoded bytes

Use only to check a deterministic producer/encoder on the same toolchain:

```bash
python3 tests/metal_rt/image_diff.py actual.png reference.png \
  --manifest reference.json --diff diff.png --metrics metrics.json \
  --exact-bytes
```

Byte-for-byte PNG equality is **not** an editor parity gate. Two PNG files can
contain identical pixels but differ in metadata or compression. Cross-GPU path
tracing can also differ slightly because of floating-point behavior. Exact
encoded comparison is useful for detecting unexpected encoder or artifact
mutation, not visual correctness.

Run the comparer self-tests with:

```bash
python3 tests/metal_rt/test_image_diff.py
```

## Reference review

Reference creation is a two-step review:

1. A candidate run captures the actual images, manifest candidate, debug
   outputs, and diff against the current reference.
2. A separate review approves the reference change and records reviewer/date.

The implementation change must not silently copy its output over the golden
image. Any reference change needs an explanation of the expected visible
difference.

## Mutation and lifetime sequences

Each relevant fixture must run both a cold-load capture and a mutation
sequence. The sequence should cover object add/remove, transform edit, material
edit, shader reload, mesh deformation, renderer restart, project reload, and
shutdown as those features become available.

For lifetime-sensitive chunks, repeat the smallest complete create/build/use/
destroy cycle 20 times locally. C18 adds a timed soak and fixed-iteration
stress run. Record iteration, peak memory, AS allocation counts, shader-cache
counts, and the first validation/device error.

## Hardware lanes

- Hosted macOS: build/smoke/unit/fallback only; no implied GPU correctness.
- Self-hosted M1 family: required baseline editor-scene lane.
- Self-hosted M2 or newer family: required second GPU-family lane by C18.
- Intel Mac/x86_64: build or non-Metal fallback evidence only.

A universal binary does not replace a named GPU lane. Skips use the standard
machine-readable reasons and are failures when the lane is configured as a
required RT runner.

## Artifact bundle

An editor-scene result contains:

- `summary.json` and exact command log;
- `capability_record.json`;
- editor/project log and shader diagnostics;
- fixture manifest and imported asset hashes;
- actual, reviewed reference ID, visual diff, and metrics for every output;
- optional screenshot/video for human diagnosis (never the sole assertion);
- resource/update counters and, when required, stability/performance JSON;
- Git commit and dirty-tree state.

The future canonical runner stages are `editor-scene`, `export`, and
`stability`. They should be added to `tests/metal_rt/run_mac_rt_tests.py` only
when the corresponding executable fixture exists.
