# arm64 Metal-only build lane (chunk C3)

## Purpose

Every existing macOS CI build installs Vulkan/MoltenVK, ANGLE, and AccessKit
opportunistically, so a regression that leaves the Metal driver unbuildable or
unbootable on its own can hide behind those fallbacks. The Metal ray tracing
port changes `drivers/metal` continuously, so the port needs one lane where the
Metal rendering driver is the **only** native rendering backend and any
breakage is attributable to it.

This lane covers test layers L1 (build), L2 (engine smoke), and L3 (unit) for
the configuration the RT port actually targets: macOS arm64, Metal enabled,
Vulkan and ANGLE excluded at compile time.

## Canonical configuration

```text
platform=macos target=editor arch=arm64 dev_mode=yes
metal=yes vulkan=no angle=no accesskit=no
```

Rationale for each pin:

- `arch=arm64` — the Metal driver is disabled by `platform/macos/detect.py` on
  any other architecture; arm64 is the only architecture where this lane is
  meaningful.
- `metal=yes` — explicit even though it is the macOS default, so the lane keeps
  building Metal if defaults ever change.
- `vulkan=no` — excludes the MoltenVK path, so nothing can silently satisfy a
  rendering dependency through Vulkan translation.
- `angle=no` — excludes the GLES-over-ANGLE fallback for the same reason.
- `accesskit=no` — removes the one optional prebuilt dependency the main lane
  downloads, keeping this lane free of network-fetched SDKs.
- `dev_mode=yes` — enables warnings-as-errors and compiles the unit-test suite
  (`tests=yes`), which the smoke stage runs.

## Where it runs

### CI

`.github/workflows/macos_builds.yml` job `build-macos-metal-only`, which runs
on the same hosted arm64 macOS runner image as the existing universal-build
job. It deliberately skips the ANGLE/AccessKit/Vulkan SDK installation steps,
builds with the canonical flags, then runs `--version`, `--help`, and
`--test --force-colors` against the arm64 binary.

Artifacts uploaded per run:

- `macos-editor-metal-only` — the stripped editor binary;
- `macos-editor-metal-only-logs` — one log per smoke/unit command, uploaded
  even when a step fails.

The hosted job proves compile health and CPU-side unit behavior only. It is
not evidence of GPU-side Metal RT correctness (test layers L4+). Chunk C12's
self-hosted Apple Silicon lane provides that coverage; see
[`ci_validation.md`](ci_validation.md).

### Local

The local runner mirrors the same flags in its `build` stage and adds the
capability probe:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py --stage preflight --stage build --stage smoke --stage unit
```

Local runs write `summary.json` plus per-command logs under
`mac-rt-planning/artifacts/` (untracked).

## Pass criteria

The lane passes only when all of the following hold:

1. the SCons build exits zero with no warnings-as-errors failures;
2. `bin/godot.macos.editor.arm64` exists and is arm64 (`lipo -archs` or
   `file`);
3. `--version` and `--help` exit zero;
4. `--test --force-colors` exits zero;
5. compile and test logs are retained as artifacts.

A failure in this lane while the universal lane stays green means the breakage
is specific to the Metal-only configuration — usually a header or symbol pulled
in via a Vulkan/ANGLE-gated include path, or a `#if defined(VULKAN_ENABLED)`
guard hiding a shared dependency.
