# Next Metal RT chunks: editor integration through shipping

## Contract

C13-C18 are **Planned**. C1-C12 remain the completed backend foundation. A
chunk changes to **Available** only when its implementation, required tests,
artifacts, and fallback behavior have landed together. Scaffolding alone does
not satisfy a chunk.

The machine-readable index is [`next-chunks.json`](next-chunks.json). Run
`python3 mac-rt-planning/scripts/check_next_chunks.py` after editing these
plans.

## Sequence

| Chunk | Milestone | Depends on | What the user gains |
|---|---|---|---|
| C13 | First editor scene (HG0) | C12 | A restricted StandardMaterial3D scene appears in the Path Tracing editor view |
| C14 | Real scene geometry and lifetime | C13 | Static, deformed, and instanced scene geometry stays correct across edits |
| C15 | Alpha and custom materials | C14 | Material behavior approaches the Windows Vulkan path |
| C16 | Procedural geometry | C15 | AABB/custom-intersection content is supported or explicitly rejected |
| C17 | Mac UX and presentation | C13 | Correct denoiser defaults, capability-aware UI, and validated presentation |
| C18 | Shipping parity | C14-C17 | Exported projects, CI, soak, and performance evidence |

Detailed scope and tests live in the per-chunk files:

- [`chunks/c13-editor-hg0.md`](chunks/c13-editor-hg0.md)
- [`chunks/c14-scene-geometry.md`](chunks/c14-scene-geometry.md)
- [`chunks/c15-material-dispatch.md`](chunks/c15-material-dispatch.md)
- [`chunks/c16-procedural-geometry.md`](chunks/c16-procedural-geometry.md)
- [`chunks/c17-mac-ux-denoising.md`](chunks/c17-mac-ux-denoising.md)
- [`chunks/c18-shipping-parity.md`](chunks/c18-shipping-parity.md)

## Critical path

C13 is the only correct next implementation chunk. It should stay deliberately
narrow: HG0 opaque StandardMaterial3D, one supported light subset, one camera,
static triangle meshes, no custom spatial shader, and denoiser `None`. Its job
is to prove the complete editor route without importing every material feature
into the first change.

C14 and C15 then widen the data model. C17 can start after C13 because UI and
default behavior do not require procedural geometry, but C18 waits for all
feature chunks.

## Shared completion rule

Every chunk PR must include:

- code and generated shader changes;
- focused CPU/unit coverage where possible;
- the required GPU/editor profile from
  [`editor-scene-testing.md`](editor-scene-testing.md);
- supported and forced-disabled fallback results;
- a capability record and machine-readable summary;
- source commit and clean/dirty state;
- known unsupported behavior in the chunk document;
- no reference-image replacement without separate review.

Until C13 passes `L5-E0`, "Metal path tracing works in the editor" is not an
accurate project claim. Until C18 passes, "Metal matches Windows" is not an
accurate shipping claim.
