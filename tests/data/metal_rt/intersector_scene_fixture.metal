// Reduced from SPIRV-Cross output for scene_raytracing_compute.glsl.
// This is a source-rewrite fixture, not a standalone Metal program.
constant uint RT_FLAGS_tmp [[function_constant(0)]];
constant uint RT_FLAGS = is_function_constant_defined(RT_FLAGS_tmp) ? RT_FLAGS_tmp : 0u;

static inline __attribute__((always_inline))
bool trace_material(thread const float3& origin, thread const float3& direction, thread const float& max_distance, thread ComputeHit& hit, thread raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>& rt_query, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    rt_query.reset(raytracing::ray(origin, direction, 0.001, max_distance), tlas, 0xFFu);
    while (rt_query.next())
    {
    }
    return rt_query.get_committed_intersection_type() == raytracing::intersection_type::triangle;
}

static __attribute__((noinline))
bool trace_shadow_blocked(thread const float3& origin, thread const float3& direction, thread const float& max_distance, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data> shadow_query;
    shadow_query.reset(raytracing::ray(origin, direction, 0.001, max_distance), tlas, 0xFFu);
    while (shadow_query.next())
    {
    }
    return shadow_query.get_committed_intersection_type() == raytracing::intersection_type::triangle;
}
