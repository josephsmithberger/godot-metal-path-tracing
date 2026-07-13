# Metal RT port baseline (chunk C1)

Status: **Available** (per the status vocabulary in `mac-rt-planning/README.md`).

This document freezes the ground truth that later chunks (C2-C12) build on: the
exact fork delta, every ray-tracing-related file, the backend entry points, and
the places where macOS/Metal is currently excluded from ray tracing. Every
claim cites a commit hash or a `file:line` location so it can be re-verified
mechanically after a rebase.

Baseline captured: 2026-07-13, on macOS 26.5.2 (arm64), branch `nvidia-pt-dlss`.

## 1. Baseline identity

| Item | Value |
|---|---|
| Baseline branch | `nvidia-pt-dlss` |
| Baseline HEAD | `f99a1c1d1d` (`Update .gitignore`, local workspace commit, 1 line) |
| Merge base with upstream `master` | `a94a6daf51` (`NVIDIA: Custom workflows and localized actions`) |
| NVIDIA fork commits | `4732933359` (`NVIDIA: Dependencies`) and `ce5786d6c0` (`NVIDIA: Pathtracer + DLSS`), both authored 2026-06-16 |
| NVIDIA diff scope | **213 files changed, 23,729 insertions, 736 deletions** |

Reproduce the scope numbers:

```bash
git fetch upstream master
git merge-base upstream/master nvidia-pt-dlss        # -> a94a6daf51
git log --oneline upstream/master..ce5786d6c0        # -> exactly the two NVIDIA commits
git diff --stat 4732933359^..ce5786d6c0 | tail -1    # -> 213 files, 23729 insertions(+), 736 deletions(-)
```

Per-commit split:

- `4732933359` "NVIDIA: Dependencies": 29 files, 4,814 insertions, 10 deletions.
  Almost entirely `thirdparty/streamline` headers plus build plumbing.
- `ce5786d6c0` "NVIDIA: Pathtracer + DLSS": 184 files, 18,915 insertions,
  726 deletions. The actual engine-side path tracer, RT device API, DLSS
  integration, and editor support.

The only commit on top of the NVIDIA work is `f99a1c1d1d`, which adds
`/mac-rt-planning` to `.gitignore` (the planning workspace is deliberately
untracked). It touches no engine code, so "current branch state" and "NVIDIA
fork state" are interchangeable for engine analysis.

## 2. Ray-tracing surface added by the fork

The full 213-file list is reproduced in [Appendix A](#appendix-a-full-nvidia-diff-file-list)
and regenerated with:

```bash
git diff --name-only 4732933359^..ce5786d6c0
```

The RT-relevant subset, by layer:

### Generic RT device API (backend-agnostic)

- `servers/rendering/rendering_device_driver.h` — abstract driver interface.
  The RT section spans lines 786-846: acceleration-structure geometry/instance
  structs, `blas_create` (793), `tlas_create` (805),
  `acceleration_structure_instance_write` (806), `acceleration_structure_free`
  (807), `acceleration_structure_get_scratch_size_bytes` (808),
  `raytracing_pipeline_create` (823), `raytracing_pipeline_free` (824),
  `raytracing_pipeline_get_shader_group_handles` (826), `command_build_blas`
  (830), `command_update_blas` (833), `command_build_tlas` (834),
  `command_bind_raytracing_pipeline` (835), `command_bind_raytracing_uniform_set`
  (836), and `command_trace_rays` (845). **14 pure-virtual methods total**;
  every `RenderingDeviceDriver` subclass must provide all of them.
- `servers/rendering/rendering_device.{h,cpp}` — public `RenderingDevice` RT
  API, gated by feature checks (see section 4).
- `servers/rendering/rendering_device_commons.h` — `SUPPORTS_RAY_QUERY` (1038)
  and `SUPPORTS_RAYTRACING_PIPELINE` (1039) feature enums.
- `servers/rendering/rendering_device_graph.{h,cpp}`,
  `rendering_device_binds.h`, `rendering_shader_container.{h,cpp}` — command
  graph and shader container support for the new RT resource types.
- `doc/classes/RDAccelerationStructureGeometry.xml`,
  `doc/classes/RenderingDevice.xml` — exposed API docs.

### Path tracer (RenderingDevice renderer layer)

- `servers/rendering/renderer_rd/forward_clustered/render_raytracing.{h,cpp}` —
  the path tracer render pass.
- `servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.{h,cpp}` —
  RT scene shader variant management.
- `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.{h,cpp}` —
  integration into the forward-clustered renderer (`_setup_rt`, TLAS build,
  RT pass dispatch).
- `servers/rendering/renderer_rd/shaders/raytracing/*.glsl` + `SCsub` —
  13 GLSL files: `scene_raytracing_raygen.glsl` (raygen), hit/miss/material
  includes, BRDF, lights, samplers, and a `multimesh_merge.glsl` compute pass.
- `servers/rendering/renderer_rd/bindless_block.{h,cpp}` — bindless resource
  blocks used by the RT path.
- Supporting storage/effects churn: `mesh_storage`, `material_storage`,
  `render_scene_buffers_rd`, `render_scene_data_rd`, `copy_effects`,
  `depth_reconstruct`, `renderer_scene_render_rd`.

### Scene/editor API surface

- `scene/resources/environment.{h,cpp}` — `pathtracing_enabled` and related
  properties on `Environment` (the runtime opt-in switch, see section 4).
- `scene/3d/rt_procedural_instance_3d.{h,cpp}`,
  `editor/scene/3d/gizmos/rt_procedural_instance_3d_gizmo_plugin.{h,cpp}`,
  `doc/classes/RTProceduralInstance3D.xml` — new procedural-geometry node.
- `scene/resources/3d/*_shape_3d.{h,cpp}` (11 shape classes),
  `modules/godot_physics_3d/godot_shape_3d.{h,cpp}` — shape triangulation
  support consumed by RT procedural instances.
- `servers/rendering/shader_language.*`, `shader_compiler.*`,
  `shader_preprocessor.*`, `shader_types.cpp` — shader-language extensions for
  RT stages.

### NVIDIA/Windows-only components (not portable, out of port scope)

- `thirdparty/streamline/include/*` (24 headers), `drivers/streamline/*`
  (7 files), `doc/classes/Streamline.xml` — Streamline/DLSS SDK integration.
- `servers/rendering/renderer_rd/effects/dlss.{h,cpp}` — DLSS effect wired to
  Streamline.
- `drivers/aftermath/*` (7 files) — Nsight Aftermath crash dumps.
- `drivers/d3d12/d3d12_pix_markers.*`, `drivers/windows/*` — Windows debugging
  and file-access support.

### Per-backend driver changes

- `drivers/vulkan/rendering_device_driver_vulkan.{h,cpp}` (+297 lines in the
  fork), `rendering_context_driver_vulkan.cpp`, `drivers/vulkan/SCsub` — the
  only real RT implementation (see section 3).
- `drivers/d3d12/rendering_device_driver_d3d12.{h,cpp}` — interface kept in
  sync; RT methods are stubs (see section 3).
- `drivers/metal/rendering_device_driver_metal.{h,cpp}` — the fork's **entire**
  Metal touch is one added stub, `command_update_blas` (4 lines in the `.cpp`,
  1 declaration in the `.h`), to keep Metal compiling after the generic API
  gained BLAS refit. No functional Metal changes.
- `drivers/gles3/rasterizer_scene_gles3.{h,cpp}`,
  `servers/rendering/dummy/rasterizer_scene_dummy.h` — interface sync only;
  GLES3/dummy do not implement `RenderingDeviceDriver` and have no RT path.

## 3. Backend entry-point status

All three RenderingDevice backends declare the full 14-method RT surface. Their
implementations differ sharply:

| Backend | Status | Evidence |
|---|---|---|
| Vulkan | **Implemented**, but compiled out on macOS/iOS | Real implementations from `blas_create` (`drivers/vulkan/rendering_device_driver_vulkan.cpp:6450`) through `command_trace_rays` (`:6728`), each body wrapped in `#if VULKAN_RAYTRACING_ENABLED`. That macro is defined to `0` when `MACOS_ENABLED` or `IOS_ENABLED` is set, with the comment "Disable raytracing support on macOS and iOS due to MoltenVK limitations" (`:57-61`). `has_feature` reports `SUPPORTS_RAY_QUERY` / `SUPPORTS_RAYTRACING_PIPELINE` from queried extension support (`:7632-7635`). |
| D3D12 | **All 14 methods are stubs** | `drivers/d3d12/rendering_device_driver_d3d12.cpp:5547-5606`, every body is `ERR_FAIL*_MSG(... "Ray tracing is not currently supported by the D3D12 driver.")`. `has_feature` has no `SUPPORTS_RAY*` case, so both report `false`. |
| Metal | **All 14 methods are stubs** | `drivers/metal/rendering_device_driver_metal.cpp:2271-2334` (`#pragma mark - Raytracing`), every body is `ERR_FAIL*_MSG(... "Ray tracing is not currently supported by the Metal driver.")`. Declarations at `drivers/metal/rendering_device_driver_metal.h:461-484`. `has_feature` (`.cpp:2678-2701`) has no `SUPPORTS_RAY*` case; the `default: return false` branch reports RT unavailable. |

> **Correction to `mac-rt-planning/deep-research-report.md`:** the porting-analysis
> diagram shows both a "Vulkan RT path" and a "D3D12 RT path" as existing. As of
> this baseline, **only Vulkan has an implementation**; D3D12 is stubbed exactly
> like Metal. The Vulkan path on Windows/Linux is therefore the sole working
> reference implementation for the Metal port.

Net effect for macOS: there is **no ray-tracing path at all** on this branch —
native Metal RT is stubbed, and the MoltenVK/Vulkan fallback is explicitly
compiled out.

The 14 Metal stub methods to be implemented, in `drivers/metal/rendering_device_driver_metal.cpp`:

| Method | Line |
|---|---|
| `blas_create` | 2275 |
| `tlas_create` | 2279 |
| `acceleration_structure_instance_write` | 2283 |
| `acceleration_structure_free` | 2287 |
| `acceleration_structure_get_scratch_size_bytes` | 2291 |
| `raytracing_pipeline_create` | 2297 |
| `raytracing_pipeline_free` | 2301 |
| `raytracing_pipeline_get_shader_group_handles` | 2305 |
| `command_build_blas` | 2311 |
| `command_update_blas` | 2315 |
| `command_build_tlas` | 2319 |
| `command_bind_raytracing_pipeline` | 2323 |
| `command_bind_raytracing_uniform_set` | 2327 |
| `command_trace_rays` | 2331 |

## 4. Runtime gating chain

How a scene actually reaches (or fails to reach) the RT path, highest layer
first:

1. **Per-Environment opt-in.** The path tracer only engages when the rendered
   `Environment` has `pathtracing_enabled` set
   (`scene/resources/environment.h:194,389-390`; consumed at
   `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp:1893`).
2. **Device capability check.** `RenderForwardClustered::_setup_rt()`
   (`render_forward_clustered.cpp:5611-5628`) returns `false` with
   `WARN_PRINT_ONCE("Raytracing not supported on this device.")` unless
   `RD::get_singleton()->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE)`. On
   Metal this is always false today, so macOS falls back to the raster path
   with only that one-time warning. This is the observable baseline behavior
   that C11 later replaces with richer gating/logging.
3. **RenderingDevice validation.** Public RT entry points in
   `servers/rendering/rendering_device.cpp` fail with "The current rendering
   device has neither raytracing pipeline nor ray query support." when neither
   feature bit is set (e.g. `:309`, `:465`, `:475`, `:727`, `:1555`, `:5224`,
   `:6482`), so script-level misuse cannot reach the driver stubs.
4. **Driver stubs.** If anything did reach the Metal driver, each method fails
   with the explicit stub message (section 3).

Additional engine-side RT plumbing added by the fork: an RT validation toggle
`Engine::is_raytracing_validation_enabled` (`core/config/engine.{h,cpp}`,
plumbed from the `--raytracing-validation` command line in `main/main.cpp`),
used by the Vulkan context to enable `VK_NV_ray_tracing_validation`.

## 5. macOS/Metal exclusions inventory

Everything that currently keeps ray tracing (and adjacent NVIDIA features) off
macOS:

| Exclusion | Location | Effect |
|---|---|---|
| Metal driver RT stubs | `drivers/metal/rendering_device_driver_metal.cpp:2271-2334` | Native Metal RT unimplemented; the central gap this port fills. |
| Metal `has_feature` omission | `drivers/metal/rendering_device_driver_metal.cpp:2678-2701` | `SUPPORTS_RAY_QUERY` / `SUPPORTS_RAYTRACING_PIPELINE` report `false`; engine falls back before touching stubs. |
| Vulkan RT compiled out on Apple platforms | `drivers/vulkan/rendering_device_driver_vulkan.cpp:57-61` | `VULKAN_RAYTRACING_ENABLED 0` under `MACOS_ENABLED`/`IOS_ENABLED` ("MoltenVK limitations"), so MoltenVK is not an RT fallback either. |
| Metal driver is arm64-only | `platform/macos/detect.py:309-311` | `metal=yes` on `x86_64` prints a warning and force-disables Metal; Intel macOS has no Metal backend at all. Metal builds define `METAL_ENABLED`/`RD_ENABLED` and link `Metal`, `MetalKit`, `MetalFX` (`:313-318`). |
| Streamline (DLSS) is Windows-only | `SConstruct:608-612` | `STREAMLINE_ENABLED` only defined for `platform == "windows"`; silently forced off elsewhere. DLSS is not part of the Metal RT port scope. |
| Aftermath is Windows-only | `SConstruct:614-634` | Force-disabled off Windows ("Aftermath is Windows-only"). |
| macOS CI has no RT coverage | `.github/workflows/macos_builds.yml` | Hosted `macos-26` runner builds x86_64 + arm64, lipo-merges, runs `--version`/`--help`/`--test --force-colors`. Compile and unit health only; no GPU, capability, or image validation (addressed by C3/C12). |

## 6. Re-verification checklist

Run after any rebase or upstream sync to confirm this baseline still holds; if
any check fails, update this document before continuing chunk work.

```bash
# 1. Fork scope is still exactly two NVIDIA commits / 213 files.
git log --oneline $(git merge-base upstream/master HEAD)..ce5786d6c0
git diff --stat 4732933359^..ce5786d6c0 | tail -1

# 2. Metal RT methods are still stubs (expect 14).
grep -c "not currently supported by the Metal driver" \
  drivers/metal/rendering_device_driver_metal.cpp

# 3. D3D12 is still stubbed / Vulkan still implemented (expect 15 and 0).
#    D3D12 is 15, not 14: the 14 RT methods plus one rejection of
#    PIPELINE_TYPE_RAYTRACING in command_bind_push_constants (:4224).
grep -c "Ray tracing is not currently supported by the D3D12 driver" \
  drivers/d3d12/rendering_device_driver_d3d12.cpp
grep -c "Ray tracing is not currently supported by the Vulkan driver" \
  drivers/vulkan/rendering_device_driver_vulkan.cpp || true

# 4. Vulkan RT still compiled out on Apple platforms.
grep -n -A4 "Disable raytracing support on macOS" \
  drivers/vulkan/rendering_device_driver_vulkan.cpp

# 5. Metal still arm64-only.
grep -n "does not support the Metal rendering driver" platform/macos/detect.py

# 6. Engine gate unchanged.
grep -n "SUPPORTS_RAYTRACING_PIPELINE" \
  servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp
```

## Appendix A: full NVIDIA diff file list

Output of `git diff --name-only 4732933359^..ce5786d6c0` (213 files):

```
.github/CODEOWNERS
.gitignore
SConstruct
core/config/engine.cpp
core/config/engine.h
core/error/error_backtrace.cpp
core/error/error_backtrace.h
core/error/error_macros.cpp
core/input/input.cpp
doc/classes/@GlobalScope.xml
doc/classes/DisplayServer.xml
doc/classes/Environment.xml
doc/classes/ProjectSettings.xml
doc/classes/RDAccelerationStructureGeometry.xml
doc/classes/RTProceduralInstance3D.xml
doc/classes/RenderingDevice.xml
doc/classes/RenderingServer.xml
doc/classes/Streamline.xml
doc/classes/Viewport.xml
drivers/SCsub
drivers/aftermath/SCsub
drivers/aftermath/aftermath.cpp
drivers/aftermath/aftermath.h
drivers/aftermath/aftermath_context.cpp
drivers/aftermath/aftermath_context.h
drivers/aftermath/aftermath_data.h
drivers/aftermath/aftermath_headers.h
drivers/d3d12/d3d12_pix_markers.cpp
drivers/d3d12/d3d12_pix_markers.h
drivers/d3d12/rendering_context_driver_d3d12.cpp
drivers/d3d12/rendering_device_driver_d3d12.cpp
drivers/d3d12/rendering_device_driver_d3d12.h
drivers/gles3/rasterizer_scene_gles3.cpp
drivers/gles3/rasterizer_scene_gles3.h
drivers/metal/rendering_device_driver_metal.cpp
drivers/metal/rendering_device_driver_metal.h
drivers/streamline/SCsub
drivers/streamline/streamline.cpp
drivers/streamline/streamline.h
drivers/streamline/streamline_context.cpp
drivers/streamline/streamline_context.h
drivers/streamline/streamline_data.h
drivers/streamline/streamline_headers.h
drivers/vulkan/SCsub
drivers/vulkan/rendering_context_driver_vulkan.cpp
drivers/vulkan/rendering_device_driver_vulkan.cpp
drivers/vulkan/rendering_device_driver_vulkan.h
drivers/windows/file_access_windows.cpp
drivers/windows/file_access_windows.h
editor/export/shader_baker_export_plugin.cpp
editor/run/game_view_plugin.cpp
editor/scene/3d/gizmos/physics/collision_shape_3d_gizmo_plugin.cpp
editor/scene/3d/gizmos/rt_procedural_instance_3d_gizmo_plugin.cpp
editor/scene/3d/gizmos/rt_procedural_instance_3d_gizmo_plugin.h
editor/scene/3d/node_3d_editor_gizmos.cpp
editor/scene/3d/node_3d_editor_gizmos.h
editor/scene/3d/node_3d_editor_plugin.cpp
editor/scene/3d/node_3d_editor_plugin.h
main/main.cpp
misc/dist/shell/_godot.zsh-completion
misc/dist/shell/godot.bash-completion
misc/dist/shell/godot.fish
misc/utility/thirdparty_fetch.py
modules/godot_physics_3d/godot_shape_3d.cpp
modules/godot_physics_3d/godot_shape_3d.h
pyproject.toml
scene/3d/rt_procedural_instance_3d.cpp
scene/3d/rt_procedural_instance_3d.h
scene/main/viewport.cpp
scene/main/viewport.h
scene/main/window.cpp
scene/register_scene_types.cpp
scene/resources/3d/box_shape_3d.cpp
scene/resources/3d/box_shape_3d.h
scene/resources/3d/capsule_shape_3d.cpp
scene/resources/3d/capsule_shape_3d.h
scene/resources/3d/concave_polygon_shape_3d.cpp
scene/resources/3d/concave_polygon_shape_3d.h
scene/resources/3d/convex_polygon_shape_3d.cpp
scene/resources/3d/convex_polygon_shape_3d.h
scene/resources/3d/cylinder_shape_3d.cpp
scene/resources/3d/cylinder_shape_3d.h
scene/resources/3d/height_map_shape_3d.cpp
scene/resources/3d/height_map_shape_3d.h
scene/resources/3d/separation_ray_shape_3d.cpp
scene/resources/3d/separation_ray_shape_3d.h
scene/resources/3d/shape_3d.cpp
scene/resources/3d/shape_3d.h
scene/resources/3d/sphere_shape_3d.cpp
scene/resources/3d/sphere_shape_3d.h
scene/resources/3d/world_boundary_shape_3d.cpp
scene/resources/3d/world_boundary_shape_3d.h
scene/resources/environment.cpp
scene/resources/environment.h
scene/resources/shader.cpp
scene/resources/shader.h
servers/display/display_server.cpp
servers/display/display_server_enums.h
servers/rendering/dummy/rasterizer_scene_dummy.h
servers/rendering/renderer_geometry_instance.h
servers/rendering/renderer_rd/bindless_block.cpp
servers/rendering/renderer_rd/bindless_block.h
servers/rendering/renderer_rd/effects/copy_effects.cpp
servers/rendering/renderer_rd/effects/copy_effects.h
servers/rendering/renderer_rd/effects/depth_reconstruct.cpp
servers/rendering/renderer_rd/effects/depth_reconstruct.h
servers/rendering/renderer_rd/effects/dlss.cpp
servers/rendering/renderer_rd/effects/dlss.h
servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp
servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h
servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp
servers/rendering/renderer_rd/forward_clustered/render_raytracing.h
servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp
servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h
servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.cpp
servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h
servers/rendering/renderer_rd/renderer_compositor_rd.cpp
servers/rendering/renderer_rd/renderer_scene_render_rd.cpp
servers/rendering/renderer_rd/renderer_scene_render_rd.h
servers/rendering/renderer_rd/shader_rd.cpp
servers/rendering/renderer_rd/shaders/SCsub
servers/rendering/renderer_rd/shaders/effects/copy_to_fb.glsl
servers/rendering/renderer_rd/shaders/effects/depth_reconstruct.glsl
servers/rendering/renderer_rd/shaders/effects/motion_vector_decode.glsl
servers/rendering/renderer_rd/shaders/fog_inc.glsl
servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl
servers/rendering/renderer_rd/shaders/raytracing/SCsub
servers/rendering/renderer_rd/shaders/raytracing/brdf_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/multimesh_merge.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_closest_hit_common_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_common_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_custom_fragment_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_custom_globals_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_data_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_hit_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_lights_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_material_eval_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/raytracing_samplers_inc.glsl
servers/rendering/renderer_rd/shaders/raytracing/scene_raytracing_raygen.glsl
servers/rendering/renderer_rd/shaders/scene_data_inc.glsl
servers/rendering/renderer_rd/storage_rd/material_storage.cpp
servers/rendering/renderer_rd/storage_rd/material_storage.h
servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp
servers/rendering/renderer_rd/storage_rd/mesh_storage.h
servers/rendering/renderer_rd/storage_rd/render_data_rd.h
servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.cpp
servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h
servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.cpp
servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h
servers/rendering/renderer_scene_cull.cpp
servers/rendering/renderer_scene_cull.h
servers/rendering/renderer_scene_render.cpp
servers/rendering/renderer_scene_render.h
servers/rendering/renderer_viewport.cpp
servers/rendering/renderer_viewport.h
servers/rendering/rendering_device.cpp
servers/rendering/rendering_device.h
servers/rendering/rendering_device_binds.h
servers/rendering/rendering_device_commons.cpp
servers/rendering/rendering_device_commons.h
servers/rendering/rendering_device_driver.h
servers/rendering/rendering_device_graph.cpp
servers/rendering/rendering_device_graph.h
servers/rendering/rendering_method.h
servers/rendering/rendering_server.cpp
servers/rendering/rendering_server.h
servers/rendering/rendering_server_default.h
servers/rendering/rendering_server_enums.h
servers/rendering/rendering_shader_container.cpp
servers/rendering/rendering_shader_container.h
servers/rendering/shader_compiler.cpp
servers/rendering/shader_compiler.h
servers/rendering/shader_language.cpp
servers/rendering/shader_language.h
servers/rendering/shader_preprocessor.cpp
servers/rendering/shader_preprocessor.h
servers/rendering/shader_types.cpp
servers/rendering/storage/environment_storage.cpp
servers/rendering/storage/environment_storage.h
servers/rendering/storage/material_storage.h
servers/rendering/storage/render_scene_buffers.cpp
servers/rendering/storage/render_scene_buffers.h
tests/scene/test_concave_polygon_shape_3d.cpp
thirdparty/glslang/SPIRV/SpvBuilder.cpp
thirdparty/glslang/patches/0003-skip-debug-info-for-opaque-types.patch
thirdparty/re-spirv/re-spirv.cpp
thirdparty/spirv-reflect/patches/0003-add-SpvOpTypeHitObjectEXT.patch
thirdparty/spirv-reflect/spirv_reflect.c
thirdparty/streamline/include/sl.h
thirdparty/streamline/include/sl_appidentity.h
thirdparty/streamline/include/sl_consts.h
thirdparty/streamline/include/sl_core_api.h
thirdparty/streamline/include/sl_core_types.h
thirdparty/streamline/include/sl_deepdvc.h
thirdparty/streamline/include/sl_device_wrappers.h
thirdparty/streamline/include/sl_directsr.h
thirdparty/streamline/include/sl_dlss.h
thirdparty/streamline/include/sl_dlss_d.h
thirdparty/streamline/include/sl_dlss_g.h
thirdparty/streamline/include/sl_helpers.h
thirdparty/streamline/include/sl_helpers_vk.h
thirdparty/streamline/include/sl_hooks.h
thirdparty/streamline/include/sl_matrix_helpers.h
thirdparty/streamline/include/sl_nis.h
thirdparty/streamline/include/sl_nvperf.h
thirdparty/streamline/include/sl_pcl.h
thirdparty/streamline/include/sl_reflex.h
thirdparty/streamline/include/sl_result.h
thirdparty/streamline/include/sl_security.h
thirdparty/streamline/include/sl_struct.h
thirdparty/streamline/include/sl_template.h
thirdparty/streamline/include/sl_version.h
```
