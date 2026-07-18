# C16: procedural geometry and custom intersections

**Status:** Planned

## Goal

Support the repository's procedural AABB/custom-intersection path on Metal, or
establish a precise user-visible unsupported contract if a case cannot be
represented safely.

## Required implementation

1. Map procedural AABB geometry into the Metal acceleration-structure path.
2. Define custom intersection dispatch for the compute/ray-query lane and its
   interaction with C15 material dispatch.
3. Validate primitive, geometry, instance, and user-ID propagation.
4. Cover bounds updates, empty bounds, invalid bounds, instance removal, and
   resource teardown.
5. Prevent unsupported procedural content from corrupting or disabling valid
   triangle content in the same scene.

## Required tests

- L3 AABB conversion and validation cases.
- L4 procedural hit/miss, mixed triangle/procedural scene, and bounds update.
- L5-E3 procedural fixture capture plus hit-kind/ID debug output.
- Invalid/unsupported procedural asset test with a single stable warning.
- Twenty update/destroy iterations with Metal validation.

## Acceptance markers

- `METAL_RT_C16_PROCEDURAL=passed`
- `METAL_RT_CUSTOM_INTERSECTION=passed`
- `METAL_RT_MIXED_GEOMETRY=passed`

## Artifacts

Support matrix, validation log, debug output, images/diffs/metrics, update
counters, and fallback diagnostic.
