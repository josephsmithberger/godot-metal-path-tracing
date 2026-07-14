# Metal TLAS implementation (C6)

Chunk C6 implements native Metal top-level acceleration-structure builds and
the Godot-to-Metal instance translation needed to reference C5 BLAS resources.
It does not add ray dispatch or advertise ray-tracing pipeline support.

## Instance record

Metal's macOS 11 TLAS descriptor identifies each BLAS with an index into an
`instancedAccelerationStructures` array, while Godot supplies a backend BLAS
handle when it asks the driver to write an instance. The Metal backend writes a
128-byte record:

- bytes 0-63 are binary-compatible with
  `MTLAccelerationStructureInstanceDescriptor`;
- the remaining bytes retain the backend BLAS handle, Godot instance ID, and
  requested mask for build-time translation and round-trip validation.

The 128-byte stride is intentional. Metal requires
`instanceDescriptorBufferOffset` to be a multiple of 64 bytes, and
`RenderingDevice` suballocates successive instance arrays from a persistent
buffer. Making every record a multiple of 64 keeps every suballocation valid.

Transforms are converted from Godot's row-addressed `Transform3D` to Metal's
column-major packed float4x3. The writer rejects non-finite transforms, unknown
instance flags, conflicting force-opaque/force-non-opaque flags, and handles
that refer to a TLAS instead of a BLAS. The flag values map directly to Metal's
instance options. Masks are retained as eight-bit Godot values.

The native macOS 11 descriptor has no user-ID field. Raising the deployment
target solely to use the macOS 12 user-ID descriptor would violate the frozen
port assumptions, so C6 keeps `AccelerationStructureInstance.id` in the driver
record. C9 must carry it explicitly through the selected shader and
resource-binding mapping.

## TLAS build

`tlas_create()` sizes and allocates the native TLAS for the declared maximum
instance count. At build time the backend:

1. validates the mapped record range, maximum count, scratch capacity, and
   referenced BLAS resources;
2. resolves each Godot BLAS handle into Metal's descriptor-array index;
3. configures the native instance buffer, offset, stride, count, and BLAS array;
4. encodes the build with the existing acceleration-structure command encoder.

A null Godot instance is made inactive with a zero native mask and references
the first valid BLAS in the array. A set containing only null instances builds
an empty TLAS. This preserves Vulkan-style null-instance behavior without
putting null objects into an `NSArray`.

## Verification

The CPU unit test checks the exact transform layout and round-trips the ID,
mask, hit-table offset, flags, and BLAS handle. It also covers non-finite
transforms, conflicting flags, and TLAS-as-BLAS rejection.

On supported Apple Silicon hardware, the `[MetalRT][GPU]` C6 smoke builds one
triangle BLAS, writes one transformed instance, builds a TLAS that references
the BLAS, checks command-buffer completion and descriptor wiring, and verifies
the mapped instance fields after the build. The test repeats allocation, build,
and teardown three times and logs a line beginning:

```text
MetalRT C6 TLAS smoke: device="..." iteration=... blas_size=... tlas_size=... tlas_scratch_size=... instance_id=... mask=... hit_sbt_offset=...
```

Run it through the canonical capability-gated runner:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py \
  --stage gpu \
  --binary bin/godot.macos.editor.arm64
```
