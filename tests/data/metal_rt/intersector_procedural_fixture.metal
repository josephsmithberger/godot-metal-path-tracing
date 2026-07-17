// Procedural variants intentionally retain intersection_query traversal.
constant uint RT_FLAGS_tmp [[function_constant(0)]];
constant uint RT_FLAGS = is_function_constant_defined(RT_FLAGS_tmp) ? RT_FLAGS_tmp : 0u;

bool trace_material(thread const float3& origin, thread const float3& direction, thread const float& max_distance, thread ComputeHit& hit, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    query.commit_bounding_box_intersection(1.0f);
    return true;
}

bool trace_shadow_blocked(thread const float3& origin, thread const float3& direction, thread const float& max_distance, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    return false;
}
