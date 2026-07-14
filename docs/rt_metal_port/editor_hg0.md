# First Metal path-traced editor scene (C13)

Status: **Available** for the restricted HG0 scene lane. Human visual review of
the fixture is still required before widening the scope beyond C13.

C13 connects real Forward+ scene data to the Metal compute/ray-query path. On
Metal, `SceneShaderRaytracing` now builds a monolithic compute shader bundle
that consumes the same camera/environment uniform, TLAS, geometry and motion
tables, material data, bindless textures, light buffer, and render targets as
the native Vulkan ray-tracing stages. Vulkan keeps its existing ray-generation,
miss, hit, and callable pipeline.

The scene route is enabled only after the compute shader and compatibility
pipeline bundle are valid. Generic `SUPPORTS_RAY_QUERY` is therefore necessary
but not sufficient. Failure to compile or create the bundle leaves Forward+
on its raster route.

## HG0 contract

The initial Metal lane intentionally supports only:

- static triangle meshes with opaque `StandardMaterial3D` surfaces;
- albedo color and texture, normal map, ORM inputs, emission, metallic,
  roughness, and specular;
- procedural-sky misses and one uploaded Forward+ light subset (the E0 fixture
  uses a directional light);
- next-event-estimation shadow queries and diffuse/specular BRDF bounces;
- depth and velocity outputs needed by the shared render path;
- denoiser `None`, with DLSS Ray Reconstruction and shader execution reordering
  masked off.

Alpha-tested or transparent surfaces, custom spatial shaders, skinned,
blend-shape, or otherwise deformed meshes, `MultiMesh`, procedural AABBs, fog,
native denoising, export templates, and broad performance claims are outside
C13. The runtime emits this restriction as a warning instead of silently
presenting those features as supported.

## Fixture and automation

The deterministic fixture is
`tests/metal_rt/editor/fixtures/e0_hg0.tscn`. It uses a fixed camera, a
checker-textured plane, a red rough box, a blue metallic sphere, a procedural
sky, four samples per pixel, two bounces, and no denoiser. The editor tool
captures the actual 3D editor viewport at 64x64, switches to an instance-ID
mask, and records device and render settings in JSON.

Run the complete C13 contract against an existing Metal editor:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage editor-scene --binary bin/godot.macos.editor.arm64
```

The stage performs a cold editor capture, a fresh-process reload capture, an
exact decoded-pixel comparison with semantic scene checks, and a forced
`GODOT_MTL_DISABLE_RAYTRACING=1` launch. Metal API validation is enabled for
all three editor launches. The required supported-route markers are:

```text
METAL_RT_EDITOR_ROUTE=compute_ray_query
METAL_RT_DENOISER=none
METAL_RT_C13_EDITOR_HG0=passed
```

The artifact directory contains both beauty images, both instance-ID masks,
their manifests, zero-difference images, comparison metrics, editor logs, the
capability record, and the command summary. The cold capture is the reference
for reload persistence; it is not a cross-device reviewed golden.

## Manual acceptance

Open the fixture in the editor with the Path Tracing view active and confirm:

- the checker plane, red box, and blue metallic sphere all appear in the fixed
  camera composition;
- the box and sphere cast plausible directional shadows, the sphere has a
  tighter metallic highlight than the box, and the sky fills miss pixels;
- the image does not flicker, corrupt, or turn black while orbiting the camera
  or after closing and reopening the project;
- the Instance ID debug view gives the plane, box, and sphere stable distinct
  colors and keeps the miss region black;
- the environment reports denoiser `None`, and no DLSS-RR or SER presentation
  is implied;
- with `GODOT_MTL_DISABLE_RAYTRACING=1`, the scene remains usable through the
  raster fallback and the C13 success marker is absent.

These appearance and interactive-editor judgments are the part automation
cannot approve on the user's behalf.

C14 subsequently widens the live Metal geometry path to static compressed and
non-indexed surfaces, deformed meshes, MultiMesh, negative scale, and scene
mutation. Its current contract and E1 fixture are documented in
[`scene_geometry.md`](scene_geometry.md); the exclusions above remain the
historical boundary of the C13 test itself.
