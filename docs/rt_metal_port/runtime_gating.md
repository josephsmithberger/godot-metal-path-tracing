# Metal ray tracing runtime gate and fallback (C11)

Status: **Available** (per the status vocabulary in
`mac-rt-planning/README.md`).

Chunk C11 replaces C10's default-off debug exposure with a runtime capability
gate. The Metal driver reports `SUPPORTS_RAY_QUERY` only when the implemented
compute lane can satisfy its complete resource-binding contract. When any
requirement is absent, the feature remains false and Godot continues through
the existing non-ray-traced renderer without creating Metal acceleration
structures.

`SUPPORTS_RAYTRACING_PIPELINE` deliberately remains false. Metal runs the path
as a ray-query compute kernel. C13 adds a separate compute
`SceneShaderRaytracing` bundle and an explicit readiness check, so restricted
HG0 editor scenes can use the query lane without advertising native RT stages
that SPIRV-Cross cannot compile.

## Gate contract

The gate requires all of the following:

| Requirement | Engine check | Why it is required |
|---|---|---|
| Platform scope | macOS arm64 Metal driver | C1 shipping scope; other Apple platforms remain unvalidated |
| Project opt-in | `rendering/pathtracer/metal_ray_query_backend` (default `true`) | Provides a restart-time escape hatch |
| No forced disable | `GODOT_MTL_DISABLE_RAYTRACING` is not `1` | Makes the fallback deterministic and automation-friendly |
| Native RT | `supportsRaytracing` | Acceleration structures and compute intersector |
| Function pointers | `supportsFunctionPointers` | Full mapped pipeline/function-table resource contract |
| Shader-visible instance IDs | UserID instance descriptors available | Material/geometry indexing from TLAS instances |
| GPU addresses | `supports_gpu_address` | Buffer-reference material records |
| Encoder-free tier-2 argument buffers | `argument_buffers_enabled()` | Bindless runtime-sized texture arrays |
| MSL RT baseline | target MSL version at least 2.3 | Ray-query/intersector language support |

The GPU-address and encoder-free argument-buffer checks make macOS 13 the
effective runtime floor for the path-tracer binding model while preserving the
macOS 11 build floor for the engine and acceleration-structure layer.

Explicit opt-outs take precedence over capability diagnostics, so a forced
fallback produces a stable reason on every machine. Capability failures are
otherwise accumulated and reported together rather than hiding the next
missing requirement behind the first one.

## Log contract

A supported launch emits:

```text
Metal ray tracing: enabled (compute ray-query backend; fallback renderer remains available). C11_GATE=enabled
```

An unsupported or forced-disabled launch warns with a human-readable reason,
states that the non-RT fallback is active, and ends with machine-readable
reason codes. For example:

```text
Metal ray tracing: disabled (GODOT_MTL_DISABLE_RAYTRACING=1); using non-RT rendering fallback. C11_GATE=disabled:forced_disabled
```

## Verification

The focused CPU tests cover a complete supported capability set, every missing
capability, the project opt-out, and the environment override:

```bash
./bin/godot.macos.editor.arm64 --test \
  '--test-case=*[MetalRT] C11*' --force-colors
```

The canonical `fallback` stage first runs the C2 capability probe, launches a
minimal mesh project and requires the supported marker, then relaunches with
the environment override and requires the fallback marker:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage fallback --binary bin/godot.macos.editor.arm64
```

Both launches must exit zero. The runner fails if either expected log marker
is absent and retains the commands, environment override, and logs in its
artifact summary.

The current local device evidence is Apple M5 on macOS 26.5.2. M1/M2 coverage
remains a named hardware-lane gap; the gate uses runtime queries rather than
GPU-family comparisons so newer families do not require source updates.
