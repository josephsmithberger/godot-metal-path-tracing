# Minimal Metal trace execution path (C8)

Status: **Available** (per the status vocabulary in
`mac-rt-planning/README.md`).

Chunk C8 connects the C5 BLAS and C6 TLAS to the native-MSL lane selected in
C7. It provides the first backend-owned Metal RT pipeline state, creates and
binds a pipeline-specific intersection-function table, dispatches a trace
kernel, and writes deterministic RGBA8 output.

The C8 test remains intentionally below the public `RenderingDevice`
ray-tracing API. C9 now maps Godot shader groups, SBT records, pipeline
resources, and uniform-set binds; consuming those records in the path-tracer
compute dispatch remains C10.

## Pipeline and bindings

`MDRaytracingPipeline::create_trace_one_ray()` runtime-compiles a native MSL
2.3 compute kernel, creates its `MTLComputePipelineState`, and allocates a
one-entry `MTLIntersectionFunctionTable` from that exact pipeline. Entry zero is
Metal's system opaque-triangle intersection function with triangle-data and
instancing signatures. The kernel receives and uses that table when it invokes
the intersector.

The C8 binding contract is deliberately small:

| Buffer index | Resource |
|---|---|
| 0 | One-instance TLAS |
| 1 | RGBA8 output image buffer |
| 2 | Width/height constants |
| 3 | Intersection-function table |

`encode_trace_one_ray()` validates all required objects, dimensions, and output
capacity before binding the pipeline and dispatching a nonuniform 2D thread
grid. The caller remains responsible for making BLAS resources referenced by
the TLAS resident on the compute encoder.

## Exact golden image

The focused GPU test builds the same single triangle and one-instance TLAS used
by C5-C7, then renders a 4x4 checkerboard. Every even pixel traces toward the
triangle and must produce the closest-hit color `(232, 168, 32, 255)`; every
odd pixel traces away and must produce the miss color `(16, 24, 40, 255)`.
Integer output avoids floating-point and image-codec variation.

The reviewed 64-byte RGBA8 reference has FNV-1a 64 hash:

```text
7d35dd82cbfd1de5
```

The test compares every pixel and the hash, checks command-buffer completion,
and repeats pipeline/table/scene allocation, dispatch, and teardown three
times. Its artifact log includes lines beginning:

```text
MetalRT C8 trace smoke: device="..." iteration=... image=4x4 ift_entries=1 golden_fnv1a64=7d35dd82cbfd1de5
```

Run it with the capability-gated GPU suite:

```bash
python3 mac-rt-planning/scripts/run_mac_rt_tests.py \
  --stage gpu \
  --binary bin/godot.macos.editor.arm64
```

Or isolate C8 after building the editor:

```bash
./bin/godot.macos.editor.arm64 --test \
  '--test-case=*[MetalRT][GPU] C8*' --no-skip --force-colors
```

## Deliberate limits

- Triangle geometry only; procedural intersection functions are not populated.
- One backend-owned native MSL kernel; C9 records user shader groups and a
  software recursion budget, but the C8 kernel does not consume them.
- Exact buffer output only; the path-tracer scene and reviewed PNG reference
  arrive in C10/L5.
- The custom Godot instance ID retained in the C6 record is not shader-visible
  at the macOS 11 descriptor floor. C9 carries group/table metadata explicitly;
  C10 must expose instance metadata to the re-expressed compute shader.
