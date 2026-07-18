# C15: alpha and custom material dispatch

**Status:** Planned

## Goal

Replace the Vulkan hit-group/SBT assumptions with a Metal-compatible material
dispatch design that covers alpha-tested and custom spatial materials.

## Required implementation

1. Choose and document either generated/inlined material variants or Metal
   visible-function-table dispatch, including cache keys and fallback behavior.
2. Implement opacity/any-hit-equivalent behavior for cutout materials.
3. Map custom shader uniforms, textures, specialization flags, and material
   identity to the compute ray-query lane.
4. Handle shader recompilation, failed compilation, cache invalidation, and
   hot reload without stale function-table references.
5. Preserve opaque HG0 as the fallback for only the cases where that behavior
   is semantically safe; otherwise reject with an actionable diagnostic.

## Required tests

- L3 material key, uniform/texture binding, and invalidation tests.
- L4 opaque, alpha cutout, double-sided, textured, and custom shader cases.
- L5-E2 material matrix capture with object/material ID debug outputs.
- Edit shader source and uniforms live, then capture the updated result.
- Failed-custom-shader test proving deterministic fallback or exclusion.
- Twenty compile/bind/destroy iterations with Metal validation.

## Acceptance markers

- `METAL_RT_C15_MATERIAL_DISPATCH=passed`
- `METAL_RT_ALPHA_TEST=passed`
- `METAL_RT_CUSTOM_SHADER_RELOAD=passed`

## Artifacts

Material support matrix, compiled-variant/cache statistics, validation log,
actual/reference/diff/metrics images, and hot-reload summary.

## Not in C15

Procedural AABB intersection is C16. A material feature not represented in the
fixture is not implicitly supported.
