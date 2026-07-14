# Godot RT pipeline mapping for Metal (C9)

Status: **Available** (per the status vocabulary in
`mac-rt-planning/README.md`).

Chunk C9 maps Godot's Vulkan-shaped ray-tracing pipeline inputs onto the
compute-backed Metal resources introduced in C8. It does not pretend that a
Vulkan Shader Binding Table (SBT) has a native Metal equivalent: every
non-1:1 rule is explicit below.

## Pipeline resource mapping

`MDRaytracingPipeline::configure_shader_groups()` validates the shader index
and stage for every synthetic group and preserves Godot's required group order:

1. ray-generation groups;
2. miss groups;
3. hit groups.

That order is the contract used by `RenderingDevice` when it calculates the
raygen, miss, and hit SBT offsets. The Metal driver reports a 16-byte group
handle, 4-byte handle alignment, and 16-byte base alignment through the
existing API traits.

Each compatibility handle is:

| Field | Meaning on Metal |
|---|---|
| `magic` | Fixed `MRTG` record discriminator |
| `group_index` | Stable index in raygen → miss → hit order |
| `intersection_function_table_index` | Metal table slot, or `UINT32_MAX` when no intersection function applies |
| `type` | Raygen, miss, triangle hit, procedural hit, or empty hit |

Handle generation clears the entire destination stride before copying the
record. This makes padding deterministic when `RenderingDevice` reuses its
thread-local SBT staging memory.

The pipeline-specific `MTLIntersectionFunctionTable` uses these rules:

- slot 0 is Metal's system opaque-triangle intersection function;
- every triangle hit group shares slot 0 because closest-hit and any-hit logic
  is inlined by the compute lowering;
- each procedural hit group reserves one stable slot beginning at 1;
- empty hit groups, used by the path tracer as sentinels, have no table slot.

Procedural slots are deliberately reserved but not filled with Vulkan
intersection shaders. SPIRV-Cross cannot lower those execution models, as C7
proved. The C10 compute lane rejects hit groups that reference shaders
(including intersection shaders); procedural dispatch stays a documented limit
of the lane ([`pathtracer_launch.md`](pathtracer_launch.md)).

## Bind order

Metal ray tracing is a compute pass. The driver therefore maps the public
pipeline and uniform-set bind commands onto the existing Metal compute state:

1. bind the `MTLComputePipelineState`;
2. bind compatible uniform sets (including acceleration structures);
3. bind push constants;
4. bind backend trace constants and the pipeline-specific intersection table;
5. dispatch the compute grid.

C9 implements steps 1–3 behind the public driver hooks. C10 completes step 5
for **compute-lane** pipelines: `command_trace_rays` dispatches the
re-expressed ray-query kernel over the pixel grid
([`pathtracer_launch.md`](pathtracer_launch.md)). Step 4 collapses on that
lane — ray-query kernels bind no intersection-function table, and the
compatibility SBT buffers are not consumed at trace time because hit logic is
inlined. Pipelines that still carry the C8 backend-owned kernel remain
rejected by the public dispatch. C11 now exposes `SUPPORTS_RAY_QUERY` through
the complete capability gate documented in
[`runtime_gating.md`](runtime_gating.md). `SUPPORTS_RAYTRACING_PIPELINE`
remains false until the scene shader bundle is re-expressed for this compute
lane.

## Non-1:1 SBT semantics

| Vulkan RT pipeline/SBT concept | Metal mapping |
|---|---|
| Opaque implementation-defined shader-group handle | Stable engine-defined 16-byte index record |
| Raygen or miss shader-group function | Inlined branch in the ray-query compute kernel; no visible-function-table slot |
| Triangle hit group | System opaque-triangle function at intersection-table slot 0 plus inlined hit logic |
| Procedural hit group | Reserved intersection-table slot plus C10 compute lowering |
| Empty hit-group sentinel | Record with no function-table slot |
| SBT device address | Compatibility buffer and byte offset retained by `RenderingDevice`; not a Metal function pointer |
| SBT stride | Record stride validated by the driver; padding is ignored by the Metal mapping |
| Pipeline recursion depth | Stored software recursion budget; the compute-lane kernel enforces it as its bounce/segment budget (C10 controlled kernel does; the engine kernel must, when it lands) |

The fork currently requests recursion depth 2. Metal's intersector has no
pipeline recursion-limit property, so accepting and storing the requested
value is more accurate than claiming a Metal hardware limit.

## Acceleration-structure uniforms

C9 also completes the C7 ray-query prerequisite:

- `RenderingShaderContainerMetal` reflects
  `UNIFORM_TYPE_ACCELERATION_STRUCTURE` into Metal's buffer-index namespace;
- classic bindings use `setAccelerationStructure` on the compute encoder;
- tier-2 argument buffers store the acceleration structure's GPU resource ID;
- resource tracking marks the acceleration structure read-only for the compute
  stage;
- the vendored SPIRV-Cross padding lookup recognizes acceleration structures as
  buffer-index resources.

The last change is intentionally two narrow switch cases. It preserves the
container's existing padded argument-buffer layout instead of disabling
padding for RT shaders.

## Verification

The CPU tests create a pipeline from synthetic shader groups and verify stage
validation, stable group order, exact handle bytes, zeroed stride padding,
function-table indices, empty sentinels, and recursion-budget validation:

```bash
./bin/godot.macos.editor.arm64 --test \
  '--test-case=*[MetalRT] C9*' --force-colors
```

The capability-gated GPU case creates the mapped compute pipeline and a real
two-entry Metal intersection-function table (opaque triangle plus one reserved
procedural slot):

```bash
python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage gpu --binary bin/godot.macos.editor.arm64
```

Its log contains a line beginning:

```text
MetalRT C9 pipeline mapping: device="..." groups=6 raygen=2 miss=1 hit=3 ift_entries=2 recursion_budget=2
```

The C7 focused test now also proves that padded argument-buffer ray-query
lowering succeeds rather than pinning the pre-C9 failure.
