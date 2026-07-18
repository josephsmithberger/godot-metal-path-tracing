# C14: real scene geometry and lifetime

**Status:** Planned

## Goal

Make the Metal editor path robust across the geometry types and mutations a
normal scene produces, without stale acceleration structures or leaked GPU
resources.

## Required implementation

1. Cover static indexed/non-indexed meshes and the vertex/index formats used by
   Forward+ scene storage, including compressed formats or an explicit
   conversion path.
2. Rebuild or update BLAS for skinned, blend-shape, and otherwise deformed
   meshes with correct synchronization.
3. Cover MultiMesh and repeated instances, transform updates, visibility
   masks, user IDs, negative scale/winding, and instance removal.
4. Define compaction/refit policy and resource ownership across editor scene
   edits, project reload, renderer restart, and shutdown.
5. Keep unsupported geometry out of the TLAS with one actionable diagnostic;
   never submit a partially initialized descriptor.

## Required tests

- L3 descriptor conversion, format, mask, user-ID, and lifetime cases.
- L4 static, deformed, and MultiMesh focused scenes.
- L5-E1 geometry matrix capture and per-object ID/debug image.
- Twenty create/edit/rebuild/destroy iterations with Metal validation.
- Remove and re-add every fixture object without restarting the editor.
- Supported and forced-disabled fallback runs.

## Acceptance markers

- `METAL_RT_C14_SCENE_GEOMETRY=passed`
- `METAL_RT_AS_REBUILD_COUNT=<integer>`
- `METAL_RT_RESOURCE_LIFETIME=passed`

## Artifacts

Geometry capability matrix, validation log, image set, rebuild/update counters,
peak-memory record, and iteration summary.

## Not in C14

Custom material dispatch and procedural intersection behavior remain C15-C16.
