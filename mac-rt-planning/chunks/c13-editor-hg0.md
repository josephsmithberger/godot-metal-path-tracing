# C13: first Metal path-traced editor scene (HG0)

**Status:** Planned

## Goal

Render a deliberately restricted real Forward+ editor scene through the Metal
compute/ray-query lane using opaque StandardMaterial3D behavior (HG0). This is
the first chunk that makes scene content visible in the editor.

## Required implementation

1. Add a Metal-compatible compute scene shader bundle that re-expresses the
   existing ray-generation loop without SPIR-V RT stages.
2. Share scene uniforms, camera/environment data, TLAS, geometry buffers,
   material tables, textures, and output resources with that bundle.
3. Add an explicit scene-shader readiness capability. Widen `_setup_rt()` to
   the ray-query route only when that readiness check succeeds; do not infer
   readiness from generic `SUPPORTS_RAY_QUERY` alone.
4. Route the Forward+ path-tracing frame to compute dispatch on Metal and keep
   the Vulkan `command_trace_rays` route unchanged.
5. Support HG0 opaque StandardMaterial3D with a documented initial light,
   environment, texture, and bounce subset.
6. Default the fixture to denoiser `None` and mask DLSS-RR/SER defines.
7. Preserve the C11 forced-disable and unsupported-device fallback.

## Required tests

- L0 generated-shader and planning checks.
- L1 arm64 Metal-only editor build; x86_64 compile/fallback lane where relevant.
- L3 layout/binding/readiness and route-selection tests.
- L4 one-frame dispatch with Metal validation and command-buffer error checks.
- L5-E0 editor fixture capture at 64x64 with fixed camera, seed, samples, and
  `None` denoiser.
- Reload the project and capture again to prove resource persistence.
- Force `GODOT_MTL_DISABLE_RAYTRACING=1` and require the fallback marker.

## Acceptance markers

- `METAL_RT_C13_EDITOR_HG0=passed`
- `METAL_RT_EDITOR_ROUTE=compute_ray_query`
- `METAL_RT_DENOISER=none`

## Artifacts

Capability record, editor log, actual/reference/diff/metrics images, scene
manifest, command summary, and a screenshot showing the fixture in the editor.

## Not in C13

Alpha testing, custom spatial shaders, skinned/deformed meshes, MultiMesh,
procedural AABBs, native denoising, export templates, or a broad performance
claim. Those omissions must remain explicit in UI/log output.
