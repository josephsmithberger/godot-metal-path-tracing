// Procedural variants intentionally retain intersection_query traversal.
constant uint RT_FLAGS_tmp [[function_constant(0)]];
constant uint RT_FLAGS = is_function_constant_defined(RT_FLAGS_tmp) ? RT_FLAGS_tmp : 0u;

bool trace_material_query(thread const float3& origin, thread const float3& direction, thread const float& max_distance, thread ComputeHit& hit, thread const uint& instance_mask, thread raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>& rt_query, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    query.commit_bounding_box_intersection(1.0f);
    return true;
}

bool trace_material(thread const float3& origin, thread const float3& direction, thread const float& max_distance, thread ComputeHit& hit, thread raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>& rt_query, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    return trace_material_query(origin, direction, max_distance, hit, 0x03u, rt_query, tlas);
}

bool trace_shadow_blocked_query(thread const float3& origin, thread const float3& direction, thread const float& max_distance, thread const uint& instance_mask, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    return false;
}

bool trace_shadow_blocked(thread const float3& origin, thread const float3& direction, thread const float& max_distance, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    return trace_shadow_blocked_query(origin, direction, max_distance, 0x03u, tlas);
}
