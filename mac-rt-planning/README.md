# macOS Metal ray tracing port plan

This directory turns the initial repository assessment into an implementation
workspace for the native Metal ray tracing port.

## Documents

- [`deep-research-report.md`](deep-research-report.md) describes the repository
  state, technical gaps, and the proposed C1-C12 work breakdown.
- [`editor-parity-findings.md`](editor-parity-findings.md) records the post-C12
  repository audit and the exact gap between backend support and editor parity.
- [`blender-apple-comparison.md`](blender-apple-comparison.md) records the
  Apple/Blender comparison of the native Metal intersector implementation,
  merge blockers, and follow-up performance work.
- [`next-chunks.md`](next-chunks.md) indexes the C13-C18 implementation sequence;
  [`next-chunks.json`](next-chunks.json) is its machine-readable contract.
- [`editor-scene-testing.md`](editor-scene-testing.md) standardizes real editor
  fixtures, captures, comparison modes, artifacts, and hardware lanes.
- [`testing-standard.md`](testing-standard.md) defines the required test layers,
  pass/fail rules, artifacts, hardware lanes, and change gates.
- [`automation.md`](automation.md) documents the local test runner and how it
  should be introduced into CI.

## Automation

`tests/metal_rt/run_mac_rt_tests.py` is the tracked canonical entry point for the test
layers that exist today. It can run preflight checks, compile and run the C2
capability probe, build an arm64 Metal-only editor, and execute the existing
version, help, and unit-test checks. Its `gpu` stage runs the focused C5
single-triangle BLAS build/refit and C6 one-instance TLAS smokes, plus the C7
hit/miss trace kernels (SPIRV-Cross-lowered ray query and native intersector),
the C8 exact-hash RGBA8 trace image, the C9 mapped pipeline/function-table
resource smoke, and the C10 controlled path-traced scene launch (compute-lane
pipeline, user-ID instances, CPU-reference image comparison with PNG
artifacts) on capable Apple Silicon hardware. B2 coverage runs the production
MSL rewrite itself as query and intersector specializations, and the dedicated
`intersector-parity` stage captures E1/E2 transform/culling/double-sided/alpha
parity under Shader Validation. Cached MSL is rejected and rebuilt when it was
compiled for the other traversal lane.
The `fallback` stage validates C11 twice: once with the supported runtime gate
and once with RT forcibly disabled, requiring a clean non-RT fallback and an
explanatory log marker. The `image` stage validates C12 against the reviewed
PNG reference and writes a visual diff plus numeric metrics. The ignored
planning-path script remains as a compatibility wrapper.
Every stage captures logs and a machine-readable summary.

Validate the C13-C18 plans and local Markdown links:

```bash
python3 mac-rt-planning/scripts/check_next_chunks.py
```

Run the canonical PNG comparison self-tests:

```bash
python3 tests/metal_rt/test_image_diff.py
```

The image-comparer self-tests are also part of the runner's `preflight` stage.
The planning check stays local because this planning directory is intentionally
ignored by the repository.

Start by inspecting the commands without running them:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py --dry-run
```

Run only the environment checks:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py --stage preflight
```

Run the C11 supported/fallback pair against an existing binary:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py \
  --stage fallback --binary bin/godot.macos.editor.arm64
```

Generated results are written under `mac-rt-planning/artifacts/` and are not
tracked by Git.

## Status vocabulary

Documents and pull requests should use these terms consistently:

- **Available**: implemented and runnable in this branch.
- **Scaffolded**: the interface or harness exists, but it does not prove RT
  correctness yet.
- **Planned**: described only; implementation has not landed.
- **Blocked**: implementation was attempted and cannot proceed without a named
  dependency, decision, or hardware resource.

The C1-C12 chunk IDs from the research report and C13-C18 IDs from the next
chunk plan are stable identifiers for port work. Test artifacts and
pull-request descriptions should name the chunk they validate.
