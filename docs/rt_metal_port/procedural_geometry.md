# Metal procedural geometry and custom intersections (C16)

Status: **Available** for validated AABB input and direct `intersection()`
bodies on the compute ray-query lane. Human review of silhouettes, normals,
hit-kind output, and live editor updates is still required.

C16 maps `RTProceduralInstance3D` bounds to Metal bounding-box BLAS geometry
and inlines each accepted custom intersection body into the same generated
compute shader used for C15 material dispatch. Vulkan retains its native
intersection-stage pipeline.

## Bounds and acceleration structures

| Input | Metal behavior |
|---|---|
| Nonempty `bounds` array | Packs every AABB as min/max float3 records with a 24-byte stride |
| Empty `bounds` array | Uses the finite, positive-volume node culling AABB as one fallback record |
| Same record count after an edit | Updates the grow-only bounds buffer and refits an `ALLOW_UPDATE` BLAS |
| Record-count or capacity change | Recreates the bounds buffer and BLAS, then performs a full build |
| Node removal or renderer teardown | Drops the TLAS record and frees BLAS before its backing bounds buffer |
| Malformed, nonfinite, flat, or inverted bounds | Frees stale procedural resources, omits that instance, and emits one stable warning |

Validation happens before Metal descriptor creation. Invalid procedural data
therefore cannot leave an old BLAS in the TLAS and cannot disable valid
triangle or procedural instances elsewhere in the scene. Bounds are exposed
to `intersection()` through `AABB_MIN` and `AABB_MAX` only when
`expose_aabb_bounds` is enabled.

## Compute custom-intersection dispatch

The append-only C15 shader slot is also the procedural dispatch identity.
Accepted slots generate an `intersect_custom_<slot>` function and a switch on
the candidate instance's material dispatch index. During
`rayQueryProceedEXT`, triangle candidates continue through alpha/material
confirmation; AABB candidates run their procedural function and call
`rayQueryGenerateIntersectionEXT` through `report_intersection`.

The generated call records the reported distance and hit kind plus
`HIT_UV`, `HIT_NORMAL`, `HIT_TANGENT`, and `PREV_POSITION`. After query
commit, the ordinary shading path receives the same geometry-table index,
primitive index, Metal UserID instance index, and material record as a
triangle hit. Primary, secondary, and shadow queries use the same procedural
dispatcher. The Hit Kind debug mode colors triangle and generated hits in
separate color families while hashing geometry and reported hit kind within
those families.

Direct `intersection()` bodies may use ordinary material uniforms and
textures. Custom stage-global helper declarations are deliberately rejected:
the monolithic compute shader still needs cross-material symbol namespacing
before those can be represented safely. A rejected procedural shader is
omitted rather than substituted with triangle/HG0 semantics.

## Verification

Focused CPU tests cover explicit and fallback bounds, malformed lengths,
flat/inverted/nonfinite input, and the procedural geometry-table ABI. The GPU
test builds a mixed triangle/AABB TLAS, refits AABB bounds, removes the
procedural instance while retaining the triangle, validates UserID and
hit-table offsets, and repeats the complete create/build/refit/remove/destroy
cycle 20 times under Metal validation:

```bash
./bin/godot.macos.editor.arm64 --test \
  '--test-case=*[MetalRT][GPU] C16*' --no-skip --force-colors
```

Run the real E3 editor fixture, cold/reload comparison, mutation sequence,
and forced raster fallback with:

```bash
python3 tests/metal_rt/run_mac_rt_tests.py \
  --stage procedural-scene --binary bin/godot.macos.editor.arm64
```

The stage captures beauty, Instance ID, Primitive ID, and Hit Kind before and
after an in-place bounds update and instance removal. It also includes an
invalid zero-volume procedural record in front of valid triangle content and
requires exactly the stable omission diagnostic. The completion markers are:

```text
METAL_RT_C16_PROCEDURAL=passed
METAL_RT_CUSTOM_INTERSECTION=passed
METAL_RT_MIXED_GEOMETRY=passed
```

## Manual acceptance

Open `tests/metal_rt/editor/fixtures/e3_procedural.tscn` with the Path Tracing
view active. Confirm that the red sphere, green two-lobe procedural object,
and yellow fallback-bounds sphere have smooth silhouettes and plausible
lighting alongside the blue triangle box and floor. The magenta invalid
procedural object must remain absent while the floor behind it stays intact.

Switch among Instance ID, Primitive ID, and Hit Kind. Instance colors should
remain attached to objects, the green object's two AABB primitives should be
distinguishable in Primitive ID, and Hit Kind should clearly separate every
procedural surface from the floor and box. After the scripted mutation, the
red procedural surface should widen and move, the green object should vanish,
and unrelated triangle content should not flash or disappear. Orbit the
camera, edit bounds repeatedly, remove/re-add nodes, and watch for stale
silhouettes, normal seams, flicker, validation messages, or steadily growing
memory. Those interactive appearance and long-session observations cannot be
approved by automated checks.
