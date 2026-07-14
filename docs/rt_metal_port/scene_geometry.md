# Metal scene geometry and lifetime (C14)

Status: **Available** for static, blend-shape/skinned deformation, repeated
MultiMesh instances, transform/visibility edits, and renderer-owned resource
teardown. Visual review of the E1 fixture is still required before starting
C15.

C14 widens the C13 Metal compute scene from static HG0 content to the geometry
that ordinary Forward+ scenes mutate. The Vulkan path keeps its existing
behavior; the shared scene builder now admits the same geometry on Metal after
the Metal descriptor layer validates the complete BLAS input.

## Geometry capability matrix

| Scene input | BLAS input | Update policy | Metal behavior |
|---|---|---|---|
| Indexed triangle list | Float3/Float2/UNORM16x4 positions plus uint16 or uint32 indices | Rebuild when the mesh surface invalidation counter changes | Supported |
| Non-indexed triangle list | Position count divisible by three | Rebuild on surface changes | Supported |
| Compressed Forward+ positions | `R16G16B16A16_UNORM`, reconstructed by the surface AABB transform | Static rebuild | Supported on the effective macOS 13 RT floor |
| Skin/blend-shape/deformed positions | Renderer-owned uncompressed copy of the current deformed vertex buffer | Full build on layout/resource changes; in-place refit on content changes | Supported |
| MultiMesh/repeated instances | Merged BLAS for compatible transforms; expanded TLAS otherwise | Refit merged data after instance edits; TLAS rebuild after composition/transform edits | Supported |
| Negative scale | Per-instance winding-bit reversal | TLAS rebuild | Supported; mixed mirrored MultiMesh content uses expanded instances |
| Point, line, or strip surface | None | None | Omitted with one conversion diagnostic |

The descriptor conversion is two-phase. Every geometry entry is checked for a
valid buffer, position format, stride/offset, primitive count, and index type
before any `MTLAccelerationStructureGeometryDescriptor` is allocated. A bad
entry therefore rejects the BLAS as a whole instead of submitting a partially
initialized descriptor.

## Synchronization and identity

Deformed vertex data is copied into renderer-owned storage before its BLAS
build/refit. The render graph tracks the producer copies and compute merge
dispatches as acceleration-structure inputs, then orders BLAS work before the
TLAS and trace dispatch. Previous-frame position storage is retained for
motion vectors.

Every TLAS record carries the current geometry-table index as the Metal UserID,
the requested 8-bit visibility mask, material hit-table offset, transform, and
winding/opacity flags. Null records are masked out. Removal and re-addition
rewrite the complete descriptor array; a stale or unbuilt BLAS makes TLAS
preparation fail before command encoding.

## Resource and compaction policy

- Static BLAS objects are owned by the surface cache and explicitly freed on
  cache cleanup, renderer restart, and shutdown. If source-resource dependency
  teardown already freed one, cleanup detects that state and does not free it
  twice.
- Deformed and merged-MultiMesh BLAS/buffer sets are owned by their dedicated
  caches, evicted after the configured TTL, and freed BLAS-first when storage
  grows or an RID is recycled.
- Per-viewport TLAS and scene tables are freed with the corresponding
  `RenderSceneBuffersRD` state.
- Command buffers retain native descriptors, acceleration structures, scratch
  storage, and referenced BLAS resources until GPU completion.
- Static builds prefer trace performance. Deformed meshes prefer build speed
  and opt into refit. MultiMesh merged geometry opts into refit and otherwise
  falls back to repeated TLAS instances.
- The backend records compacted sizes when callers request compaction, but the
  scene renderer does not replace live AS allocations yet. Replacing an AS
  would invalidate TLAS references and requires a separate graph-visible copy
  API; C14 favors deterministic ownership over an unsafe asynchronous swap.

## Verification

Focused unit tests cover descriptor formats, indexed/non-indexed counts,
invalid layouts, masks, user IDs, transforms, winding, and cache validity.
The GPU test creates static indexed, compressed non-indexed, and refittable
geometry; removes and re-adds an instance; and rebuilds/destroys the complete
set 20 times under Metal validation:

```bash
./bin/godot.macos.editor.arm64 --test \
  '--test-case=*[MetalRT][GPU] C14*' --no-skip --force-colors
```

Its required completion markers are:

```text
METAL_RT_C14_SCENE_GEOMETRY=passed
METAL_RT_AS_REBUILD_COUNT=<integer>
METAL_RT_RESOURCE_LIFETIME=passed
```

Run the real E1 editor scene, cold/reload comparison, same-process mutation,
and forced fallback with:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage geometry-scene --binary bin/godot.macos.editor.arm64
```

The artifact bundle includes beauty, instance-ID, and primitive-ID images
before and after mutation, exact reload diffs, the geometry matrix, device
record, command logs, and metrics. C15 still owns alpha, double-sided/front-
culled, and custom spatial material dispatch; C16 owns procedural AABBs.

## Manual acceptance

Open `tests/metal_rt/editor/fixtures/e1_geometry.tscn` in the Path Tracing view
and confirm that indexed, non-indexed, compressed, blend-shape, mirrored, and
all three MultiMesh objects remain visible. Watch the automated mutation or
make equivalent edits manually: change the blend-shape weight, move repeated
instances, toggle visibility, remove/re-add nodes, and reopen the project.

Confirm that geometry neither freezes in its old pose nor flashes/disappears,
mirrored objects retain the expected front faces, instance-ID regions track
objects without stale colors, primitive-ID regions remain attached to their
triangles, memory does not climb after repeated reloads, and the forced-disable
launch stays usable through raster fallback. These interactive appearance and
long-session memory judgments are not approved by the automated checks.
