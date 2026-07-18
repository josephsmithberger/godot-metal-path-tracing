# C18: exports, CI, stability, and shipping parity

**Status:** Planned

## Goal

Prove that the supported Metal path is distributable and repeatable beyond a
developer editor build.

## Required implementation

1. Include all required Metal RT shaders/assets in debug and release export
   templates and in exported projects.
2. Exercise an exported arm64 project with the same capability, editor-scene,
   fallback, and image contracts as the editor.
3. Activate and document the labelled self-hosted CI lane with clean checkout,
   timeout, concurrency, and unconditional artifact upload.
4. Run the compatibility matrix on at least an M1-family machine and an
   M2-or-newer family machine at supported macOS deployment targets.
5. Add L6 soak, memory, validation, and same-machine performance baselines.
6. Re-capture final evidence from a clean C18 commit; earlier dirty C11/C12
   artifacts are historical evidence, not a release sign-off.
7. Document fallback behavior for Intel Mac, unsupported Apple GPU/OS, and
   projects selecting unsupported features.

## Required tests

- Release and debug editor/export-template builds.
- L5-E0 through L5-E3 in editor and exported project where applicable.
- Forced-disable and unsupported-hardware fallback.
- Minimum 30-minute scene-edit/render soak and a fixed-iteration stress run.
- BLAS, TLAS, trace, denoise/presentation, and total-frame timings on the same
  named machine before/after the final change.
- Clean-tree CI rerun with no unexpected skip.

## Acceptance markers

- `METAL_RT_C18_EXPORT=passed`
- `METAL_RT_C18_CI=passed`
- `METAL_RT_C18_STABILITY=passed`
- `METAL_RT_C18_PARITY=passed`

## Artifacts

Editor and export summaries, template/build logs, capability records, complete
image set, soak/memory report, performance JSON, CI run link, source commit,
and clean-tree state.

## Completion claim

Only after this chunk is Available may the project describe the supported
Metal editor/export path as being at parity with the current Windows Vulkan
path, subject to the published feature matrix.
