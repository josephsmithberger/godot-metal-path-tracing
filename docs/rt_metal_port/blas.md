# Metal BLAS implementation (C5)

Chunk C5 implements native Metal bottom-level acceleration structures for
triangle and AABB geometry behind Godot's existing rendering-device driver
interface. It does not advertise ray-tracing pipeline support yet. TLAS builds
are implemented by [C6](tlas.md), the shader strategy by
[C7](shader_strategy.md), and the isolated native trace path by
[C8](trace_one_ray.md); public pipeline mapping remains a later chunk.

## Backend contract

- `blas_create()` converts Godot geometry descriptors, asks Metal for build and
  refit sizes, allocates an `MTLAccelerationStructure`, and retains the native
  descriptor and resources.
- The scratch-size query returns the larger of the build and refit requirement
  when `ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT` is set, allowing Godot to reuse
  one scratch buffer for either operation.
- `command_build_blas()` encodes a native Metal build. When compaction is
  allowed, it also writes the compacted size to a shared result buffer retained
  by the BLAS handle.
- `command_update_blas()` performs an in-place Metal refit and rejects BLASes
  that were not created for updates or have not had a build encoded.
- Command buffers retain the descriptor, acceleration structure, scratch
  buffer, and compacted-size buffer until their encoded work is retired.

Actual compaction into a second allocation is intentionally not performed in
C5. The queried size is bookkeeping for a later memory-policy decision; the
original acceleration structure remains valid and is used for refits.

## Supported geometry inputs

Triangle geometry accepts these Godot vertex formats; anything else fails
`blas_create()` with an explicit error rather than encoding a bad descriptor:

| Godot format | Metal attribute format | Minimum OS |
|---|---|---|
| `DATA_FORMAT_R32G32B32_SFLOAT` | `MTLAttributeFormatFloat3` (Metal's default) | macOS 11.0 / iOS 14.0 |
| `DATA_FORMAT_R32G32_SFLOAT` | `MTLAttributeFormatFloat2` | macOS 13.0 / iOS 16.0 |
| `DATA_FORMAT_R16G16B16A16_UNORM` | `MTLAttributeFormatUShort4Normalized` | macOS 13.0 / iOS 16.0 |

Non-default formats require the descriptor's `vertexFormat` property, which is
gated at runtime with an explicit failure below macOS 13 (assumption A4 in
`assumptions.md` expects macOS 13 as the effective RT floor anyway). The C7
shader audit should confirm which formats the path tracer actually emits and
extend this table if needed.

AABB geometry requires a stride of at least 24 bytes and a multiple of 4.

Device-reported scratch sizes are validated against the scratch buffer's
`length()` before builds and refits are encoded. Apple silicon may legitimately
report a `refitScratchBufferSize` of zero (measured on Apple M5 for a
single-triangle BLAS); nothing may assume the refit requirement is nonzero.

## Focused GPU smoke

Run the canonical C5 test with:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage gpu \
  --binary bin/godot.macos.editor.arm64
```

The runner first checks the machine's recorded Metal capabilities. Unsupported
machines skip with `missing_metal_rt_feature`; non-arm64 requests skip with
`unsupported_arch`. On supported hardware, the test builds and refits the same
single-triangle descriptor through three create/build/refit/destroy cycles. It
fails on a Metal command-buffer error, a zero compacted size, or a compacted
size larger than the original build allocation.

Each successful iteration logs this stable prefix and the measured sizes:

```text
MetalRT C5 BLAS smoke: device="..." iteration=... build_size=... compacted_size=... build_scratch_size=... refit_scratch_size=...
```

Metal API validation can be enabled from Xcode or the process environment for
the manual verification run. The retained runner log and `summary.json` are the
C5 L4 evidence artifacts.
