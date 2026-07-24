// Reduced from SPIRV-Cross output for scene_raytracing_compute.glsl.
// This is a source-rewrite fixture, not a standalone Metal program.
constant uint RT_FLAGS_tmp [[function_constant(0)]];
constant uint RT_FLAGS = is_function_constant_defined(RT_FLAGS_tmp) ? RT_FLAGS_tmp : 0u;

static inline __attribute__((always_inline))
bool trace_material_query(thread const float3& origin, thread const float3& direction, thread const float& max_distance, thread ComputeHit& hit, thread const uint& instance_mask, const device CurrentTransforms& _2402, const device GeometryBuffer& _2566, thread raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>& rt_query, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    rt_query.reset(raytracing::ray(origin, direction, 0.001, max_distance), tlas, instance_mask);
    while (rt_query.next())
    {
    }
    return rt_query.get_committed_intersection_type() == raytracing::intersection_type::triangle;
}

static inline __attribute__((always_inline))
bool trace_material(thread const float3& origin, thread const float3& direction, thread const float& max_distance, thread ComputeHit& hit, const device CurrentTransforms& _2402, const device GeometryBuffer& _2566, thread raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>& rt_query, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    return trace_material_query(origin, direction, max_distance, hit, 0x03u, _2402, _2566, rt_query, tlas);
}

static __attribute__((noinline))
bool trace_shadow_blocked_query(thread const float3& origin, thread const float3& direction, thread const float& max_distance, thread const uint& instance_mask, const device CurrentTransforms& _2402, const device GeometryBuffer& _2566, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data> shadow_query;
    shadow_query.reset(raytracing::ray(origin, direction, 0.001, max_distance), tlas, instance_mask);
    while (shadow_query.next())
    {
    }
    return shadow_query.get_committed_intersection_type() == raytracing::intersection_type::triangle;
}

static __attribute__((noinline))
bool trace_shadow_blocked(thread const float3& origin, thread const float3& direction, thread const float& max_distance, const device CurrentTransforms& _2402, const device GeometryBuffer& _2566, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    return trace_shadow_blocked_query(origin, direction, max_distance, 0x03u, _2402, _2566, tlas);
}
