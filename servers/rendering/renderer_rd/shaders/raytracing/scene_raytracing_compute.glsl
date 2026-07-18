#[compute]

#version 460

#extension GL_EXT_control_flow_attributes : enable

#VERSION_DEFINES

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define GLSL 1
#define RT_STAGE_COMPUTE 1
#define RT_COMPUTE_LANE 1

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "brdf_inc.glsl"
#include "raytracing_common_inc.glsl"
#include "raytracing_hit_inc.glsl"
// clang-format on

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// The binding layout intentionally matches the Vulkan scene bundle. This lets
// RenderRaytracing share camera/environment data, TLAS, geometry/material
// tables, bindless textures, samplers, and output resources across both routes.
layout(set = 0, binding = 0, rgba32f) uniform image2D image;
layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 4, std430) readonly buffer MotionIndexBuffer {
	int motion_indices[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};

layout(set = 0, binding = 32, std430) readonly buffer MotionTransforms {
	InstanceMotionData motion_transforms[];
};

// Current per-instance transforms, indexed by gl_InstanceCustomIndexEXT like
// geometries[]/materials[]. Committed-hit transforms are read from this table
// instead of the ray query so shading never depends on live query state; the
// Metal native-intersector fast path relies on that (it bypasses the query).
struct InstanceCurrentXform {
	vec4 object_to_world[3]; // Transposed 3x4 rows.
	vec4 world_to_object[3]; // Transposed 3x4 rows.
};

layout(set = 0, binding = 33, std430) readonly buffer CurrentTransforms {
	InstanceCurrentXform current_transforms[];
};

mat4 current_object_to_world(uint geometry_idx) {
	return transpose(mat4(current_transforms[geometry_idx].object_to_world[0],
			current_transforms[geometry_idx].object_to_world[1],
			current_transforms[geometry_idx].object_to_world[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
}

mat4 current_world_to_object(uint geometry_idx) {
	return transpose(mat4(current_transforms[geometry_idx].world_to_object[0],
			current_transforms[geometry_idx].world_to_object[1],
			current_transforms[geometry_idx].world_to_object[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
}

layout(set = 0, binding = 7) uniform texture2D radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

// clang-format off
#include "raytracing_samplers_inc.glsl"
#include "raytracing_material_eval_inc.glsl"
// clang-format on

struct ComputeHit {
	float t;
	uint geometry_idx;
	uint primitive_idx;
	vec2 barycentrics;
	bool front_face;
	bool procedural;
	uint hit_kind;
	vec2 procedural_uv;
	vec3 procedural_normal;
	vec3 procedural_tangent;
	vec3 procedural_prev_position;
	bool procedural_prev_position_valid;
};

struct ComputeHitData {
	vec3 hit_pos;
	vec3 geometry_normal;
	vec3 tangent;
	vec3 bitangent;
	vec2 uv;
	vec4 color;
	uint geometry_idx;
};

struct ComputeProceduralHit {
	float t;
	uint geometry_idx;
	uint primitive_idx;
	uint hit_kind;
	vec2 uv;
	vec3 normal;
	vec3 tangent;
	vec3 prev_position;
	bool prev_position_valid;
	bool valid;
};

/* RT_COMPUTE_CUSTOM_TYPES */

void load_query_candidate_hit(rayQueryEXT query, out ComputeHit hit) {
	hit.t = rayQueryGetIntersectionTEXT(query, false);
	hit.geometry_idx = rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
	hit.primitive_idx = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
	hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, false);
	hit.front_face = rayQueryGetIntersectionFrontFaceEXT(query, false);
	hit.procedural = false;
}

void load_query_committed_hit(rayQueryEXT query, out ComputeHit hit) {
	hit.t = rayQueryGetIntersectionTEXT(query, true);
	hit.geometry_idx = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
	hit.primitive_idx = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
	hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
	hit.front_face = rayQueryGetIntersectionFrontFaceEXT(query, true);
	hit.procedural = false;
	hit.hit_kind = hit.front_face ? 0xFEu : 0xFFu;
}

void load_query_committed_procedural_hit(rayQueryEXT query, ComputeProceduralHit procedural_hit, out ComputeHit hit) {
	hit.t = rayQueryGetIntersectionTEXT(query, true);
	hit.geometry_idx = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
	hit.primitive_idx = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
	hit.barycentrics = vec2(0.0);
	mat3 normal_matrix = transpose(mat3(current_world_to_object(hit.geometry_idx)));
	hit.front_face = dot(normalize(normal_matrix * procedural_hit.normal),
							 -rayQueryGetWorldRayDirectionEXT(query)) > 0.0;
	hit.procedural = true;
	hit.hit_kind = procedural_hit.hit_kind;
	hit.procedural_uv = procedural_hit.uv;
	hit.procedural_normal = procedural_hit.normal;
	hit.procedural_tangent = procedural_hit.tangent;
	hit.procedural_prev_position = procedural_hit.prev_position;
	hit.procedural_prev_position_valid = procedural_hit.prev_position_valid;
}

// The instance transforms are passed in from the current_transforms table at
// each call site instead of being copied into ComputeHit: keeping two mat4s
// (~32 scalars) in the hit struct made them live across the whole shading
// block and cost register pressure.
ComputeHitData compute_hit_data(ComputeHit hit, mat4 object_to_world, mat4 world_to_object, vec3 ray_origin, vec3 ray_direction) {
	ComputeHitData result;
	result.geometry_idx = hit.geometry_idx;
	GeometryData geometry = geometries[hit.geometry_idx];
	result.hit_pos = ray_origin + ray_direction * hit.t;

	if (hit.procedural) {
		result.uv = hit.procedural_uv;
		result.color = vec4(1.0);
		mat3 model_rotation = mat3(object_to_world);
		mat3 normal_matrix = transpose(mat3(world_to_object));
		result.geometry_normal = normalize(normal_matrix * hit.procedural_normal);
		result.tangent = normalize(model_rotation * hit.procedural_tangent);
		result.bitangent = normalize(cross(result.geometry_normal, result.tangent));
		if (!hit.front_face) {
			result.geometry_normal = -result.geometry_normal;
		}
		return result;
	}

	uint i0, i1, i2;
	get_triangle_indices_ex(geometry, hit.primitive_idx, i0, i1, i2);
	vec3 bary = vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y,
			hit.barycentrics.x, hit.barycentrics.y);
	MaterialData hit_material = materials[hit.geometry_idx];
	bool needs_full_attributes = (hit_material.flags & (RT_MAT_FLAG_HAS_NORMAL_MAP | RT_MAT_FLAG_CUSTOM_SHADER)) != 0u;
	bool needs_uv = (hit_material.flags & (RT_MAT_FLAG_HAS_NORMAL_MAP | RT_MAT_FLAG_HAS_EMISSION_TEX |
										 RT_MAT_FLAG_CUSTOM_SHADER | RT_MAT_FLAG_HAS_ALBEDO_TEX | RT_MAT_FLAG_HAS_ORM_TEX)) != 0u;
	result.uv = needs_uv ? fetch_uv(geometry, i0, i1, i2, bary) : vec2(0.0);
	result.color = needs_full_attributes ? fetch_color(geometry, i0, i1, i2, bary) : vec4(1.0);

	mat3 model_rotation = mat3(object_to_world);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));
	if (needs_full_attributes) {
		TBNResult tbn = fetch_tbn(geometry, i0, i1, i2, bary);
		result.geometry_normal = normalize(normal_matrix * tbn.normal);
		result.tangent = normalize(normal_matrix * tbn.tangent);
		result.bitangent = cross(result.geometry_normal, result.tangent) * tbn.bitangent_sign;
	} else {
		TBNResult normal_only = fetch_tbn(geometry, i0, i1, i2, bary);
		result.geometry_normal = normalize(normal_matrix * normal_only.normal);
		result.tangent = vec3(1.0, 0.0, 0.0);
		result.bitangent = vec3(0.0, 0.0, 1.0);
	}
	if (!hit.front_face) {
		result.geometry_normal = -result.geometry_normal;
	}

	return result;
}

vec4 sample_bindless_texture(uint texture_index, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(texture_index)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv);
}

vec4 sample_material_texture(uint texture_index, vec2 uv, uint material_flags) {
	if ((material_flags & RT_MAT_FLAG_POINT_FILTER) != 0u) {
		return texture(sampler2D(bindless_textures[nonuniformEXT(texture_index)], SAMPLER_NEAREST_REPEAT), uv);
	}
	return sample_bindless_texture(texture_index, uv);
}

MaterialResult evaluate_hg0(ComputeHitData hit) {
	MaterialData material = materials[hit.geometry_idx];
	vec2 uv = hit.uv * material.uv1_scale + material.uv1_offset;

	vec3 final_normal = hit.geometry_normal;
	if ((material.flags & RT_MAT_FLAG_HAS_NORMAL_MAP) != 0u) {
		vec3 tangent_normal;
		tangent_normal.xy = sample_bindless_texture(material.normal_texture_idx, uv).xy * 2.0 - 1.0;
		tangent_normal.z = sqrt(max(0.0, 1.0 - dot(tangent_normal.xy, tangent_normal.xy)));
		vec3 mapped = hit.tangent * tangent_normal.x + hit.bitangent * tangent_normal.y + hit.geometry_normal * tangent_normal.z;
		final_normal = normalize(mix(hit.geometry_normal, mapped, material.normal_map_depth));
	}

	vec4 albedo_texture = (material.flags & RT_MAT_FLAG_HAS_ALBEDO_TEX) != 0u ?
			sample_material_texture(material.albedo_texture_idx, uv, material.flags) : vec4(1.0);
	vec3 orm = (material.flags & RT_MAT_FLAG_HAS_ORM_TEX) != 0u ?
			sample_material_texture(material.orm_texture_idx, uv, material.flags).rgb : vec3(1.0);

	MaterialResult result;
	result.albedo = albedo_texture.rgb * material.albedo_color.rgb;
	result.alpha = albedo_texture.a * material.albedo_color.a;
	result.alpha_scissor_threshold = material.alpha_scissor_threshold;
	result.roughness = saturate(orm.g * material.roughness);
	result.metalness = saturate(orm.b * material.metallic);
	result.specular = material.specular;
	result.emissive = vec3(0.0);
	if ((material.flags & RT_MAT_FLAG_HAS_EMISSION_TEX) != 0u) {
		result.emissive = sample_material_texture(material.emission_texture_idx, uv, material.flags).rgb *
				material.emission_color * material.emission_strength * scene_data_block.data.emissive_exposure_normalization;
	}
	result.normal = final_normal;
	return result;
}

/* RT_COMPUTE_CUSTOM_FUNCTIONS */

bool evaluate_procedural_intersection(rayQueryEXT query, vec3 world_origin, vec3 world_direction, float max_distance, inout ComputeProceduralHit procedural_hit) {
	uint geometry_idx = rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
	MaterialData material = materials[geometry_idx];
	switch (material.dispatch_index) {
		/* RT_COMPUTE_PROCEDURAL_CASES */
		default:
			return false;
	}
}

MaterialResult evaluate_material(ComputeHit hit, ComputeHitData hit_data, mat4 object_to_world, mat4 world_to_object, vec3 ray_direction) {
	MaterialData material = materials[hit.geometry_idx];
	switch (material.dispatch_index) {
		/* RT_COMPUTE_CUSTOM_CASES */
		default:
			return evaluate_hg0(hit_data);
	}
}

// Metal ray queries expose non-opaque triangle candidates to the compute
// shader. Evaluate the selected inlined material before confirming each
// candidate: this is the any-hit equivalent for alpha-scissored surfaces.
bool ray_query_candidate_accepts(rayQueryEXT query, vec3 origin, vec3 direction) {
	// Spec-constant fold: with an all-opaque material table no candidate can
	// be rejected, so the inlined material evaluation below compiles out of
	// both traversal loops entirely.
	if ((RT_FLAGS & RT_FLAG_ALL_OPAQUE) != 0u) {
		return true;
	}
	ComputeHit candidate;
	load_query_candidate_hit(query, candidate);
	MaterialData candidate_material = materials[candidate.geometry_idx];
	bool needs_alpha_test = (candidate_material.flags & RT_MAT_FLAG_ALPHA_SCISSOR) != 0u;
	if ((candidate_material.flags & RT_MAT_FLAG_CUSTOM_SHADER) != 0u && candidate_material.dispatch_index != 0u) {
		needs_alpha_test = true;
	}
	if (!needs_alpha_test) {
		return true;
	}
	mat4 candidate_object_to_world = current_object_to_world(candidate.geometry_idx);
	mat4 candidate_world_to_object = current_world_to_object(candidate.geometry_idx);
	ComputeHitData candidate_data = compute_hit_data(candidate, candidate_object_to_world, candidate_world_to_object, origin, direction);
	MaterialResult evaluated = evaluate_material(candidate, candidate_data, candidate_object_to_world, candidate_world_to_object, direction);
	return !(evaluated.alpha_scissor_threshold > 0.0 && evaluated.alpha < evaluated.alpha_scissor_threshold);
}

// RT_RAY_FLAGS keeps back-face culling aligned with the Vulkan lanes;
// double-sided materials override it per instance via
// ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT.
// Primary traversal owns this query; shadow visibility uses its own local one.
// Sharing a single query between them made SPIRV-Cross pass it by reference into
// the shadow path, which forces the object into addressable thread memory and
// costs ~20% of the pass -- the primary traversal then pays memory traffic per
// step. Keep the two queries separate.
rayQueryEXT rt_query;

// With an all-opaque table, also traverse with the opaque ray flag so no
// triangle candidate ever surfaces to the proceed loop. Spec-constant fold.
#define RT_TRAVERSAL_FLAGS (((RT_FLAGS & RT_FLAG_ALL_OPAQUE) != 0u) ? (RT_RAY_FLAGS | gl_RayFlagsOpaqueEXT) : RT_RAY_FLAGS)

bool trace_material_query(vec3 origin, vec3 direction, float max_distance, out ComputeHit hit, uint instance_mask) {
	ComputeProceduralHit procedural_hit;
	procedural_hit.t = max_distance;
	procedural_hit.valid = false;
	rayQueryInitializeEXT(rt_query, tlas, RT_TRAVERSAL_FLAGS,
			instance_mask, origin, 0.001, direction, max_distance);
	while (rayQueryProceedEXT(rt_query)) {
		uint candidate_type = rayQueryGetIntersectionTypeEXT(rt_query, false);
		if (candidate_type == gl_RayQueryCandidateIntersectionTriangleEXT) {
			if (ray_query_candidate_accepts(rt_query, origin, direction)) {
				rayQueryConfirmIntersectionEXT(rt_query);
			}
		} else if (candidate_type == gl_RayQueryCandidateIntersectionAABBEXT) {
			evaluate_procedural_intersection(rt_query, origin, direction, max_distance, procedural_hit);
		}
	}

	uint committed_type = rayQueryGetIntersectionTypeEXT(rt_query, true);
	if (committed_type == gl_RayQueryCommittedIntersectionTriangleEXT) {
		load_query_committed_hit(rt_query, hit);
		return true;
	}
	if (committed_type == gl_RayQueryCommittedIntersectionGeneratedEXT && procedural_hit.valid) {
		load_query_committed_procedural_hit(rt_query, procedural_hit, hit);
		return true;
	}
	return false;
}

bool trace_material(vec3 origin, vec3 direction, float max_distance, out ComputeHit hit) {
	return trace_material_query(origin, direction, max_distance, hit, RT_INSTANCE_MASK_ALL);
}

// Shadow rays only need any confirmed hit, so they terminate on the first
// alpha-accepted candidate instead of resolving the closest one.
bool trace_shadow_blocked_query(vec3 origin, vec3 direction, float max_distance, uint instance_mask) {
	ComputeProceduralHit procedural_hit;
	procedural_hit.t = max_distance;
	procedural_hit.valid = false;
	rayQueryEXT shadow_query;
	rayQueryInitializeEXT(shadow_query, tlas, RT_TRAVERSAL_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT,
			instance_mask, origin, 0.001, direction, max_distance);
	while (rayQueryProceedEXT(shadow_query)) {
		uint candidate_type = rayQueryGetIntersectionTypeEXT(shadow_query, false);
		if (candidate_type == gl_RayQueryCandidateIntersectionTriangleEXT) {
			if (ray_query_candidate_accepts(shadow_query, origin, direction)) {
				rayQueryConfirmIntersectionEXT(shadow_query);
			}
		} else if (candidate_type == gl_RayQueryCandidateIntersectionAABBEXT) {
			evaluate_procedural_intersection(shadow_query, origin, direction, max_distance, procedural_hit);
		}
	}
	uint committed_type = rayQueryGetIntersectionTypeEXT(shadow_query, true);
	return committed_type == gl_RayQueryCommittedIntersectionTriangleEXT ||
			committed_type == gl_RayQueryCommittedIntersectionGeneratedEXT;
}

bool trace_shadow_blocked(vec3 origin, vec3 direction, float max_distance) {
	return trace_shadow_blocked_query(origin, direction, max_distance, RT_INSTANCE_MASK_ALL);
}

// clang-format off
#include "raytracing_lights_inc.glsl"
// clang-format on

mat4 decode_prev_object_to_world(int motion_index) {
	InstanceMotionData motion = motion_transforms[motion_index];
	return transpose(mat4(
			vec4(motion.prev_xform[0], motion.prev_xform[1], motion.prev_xform[2], motion.prev_xform[3]),
			vec4(motion.prev_xform[4], motion.prev_xform[5], motion.prev_xform[6], motion.prev_xform[7]),
			vec4(motion.prev_xform[8], motion.prev_xform[9], motion.prev_xform[10], motion.prev_xform[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
}

void write_primary_hit_outputs(uvec2 pixel, ComputeHit hit, ComputeHitData hit_data) {
	mat4 view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec4 clip = scene_data_block.data.projection_matrix * (view_matrix * vec4(hit_data.hit_pos, 1.0));
	imageStore(rt_depth_image, ivec2(pixel), vec4(clip.z / clip.w));

	int motion_index = motion_indices[hit.geometry_idx];
	mat4 previous_model = motion_index >= 0 ? decode_prev_object_to_world(motion_index) : current_object_to_world(hit.geometry_idx);
	vec3 object_position = (current_world_to_object(hit.geometry_idx) * vec4(hit_data.hit_pos, 1.0)).xyz;
	if (hit.procedural && hit.procedural_prev_position_valid) {
		object_position = hit.procedural_prev_position;
	}
	vec3 previous_world_position = (previous_model * vec4(object_position, 1.0)).xyz;
	vec2 current_uv = project_uv(hit_data.hit_pos, curr_vp_unjittered);
	vec2 previous_uv = project_uv(previous_world_position, prev_vp_unjittered);
	imageStore(rt_velocity_image, ivec2(pixel), vec4(previous_uv - current_uv, 0.0, 0.0));
}

vec3 sample_environment(vec3 ray_direction) {
	mat3 camera_basis = mat3(scene_data_block.data.inv_view_matrix);
	mat3 world_to_sky = scene_data_block.data.radiance_inverse_xform * camera_basis;
	vec3 sky_direction = world_to_sky * ray_direction;
	vec2 border = vec2(scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 sky_uv = vec3_to_oct_with_border(sky_direction, border);
	return textureLod(sampler2D(radiance_octmap, radiance_sampler), sky_uv, 0.0).rgb *
			scene_data_block.data.IBL_exposure_normalization;
}

// Chooses a diffuse or specular lobe, samples it, and advances the path state.
// Returns false when the path terminates (unrecoverable BRDF sample).
bool scatter_from_hit(vec3 hit_pos, vec3 geometry_normal, vec3 shading_normal, vec3 view_direction,
		MaterialProperties brdf_material, inout uint rng_state, inout vec3 throughput,
		inout uint diffuse_bounces, out vec3 ray_origin, out vec3 ray_direction) {
	vec3 specular_f0 = baseColorToSpecularF0(brdf_material.baseColor,
			brdf_material.metalness, brdf_material.dielectricF0);
	vec3 diffuse_reflectance = baseColorToDiffuseReflectance(brdf_material.baseColor,
			brdf_material.metalness);
	float specular_luminance = luminance(specular_f0);
	float diffuse_luminance = luminance(diffuse_reflectance);
	int brdf_type;
	if (diffuse_luminance < 0.0001) {
		brdf_type = SPECULAR_TYPE;
	} else if (specular_luminance < 0.0001) {
		brdf_type = DIFFUSE_TYPE;
	} else {
		float probability = clamp(specular_luminance / (specular_luminance + diffuse_luminance), 0.01, 0.99);
		if (rand(rng_state) < probability) {
			brdf_type = SPECULAR_TYPE;
			throughput /= probability;
		} else {
			brdf_type = DIFFUSE_TYPE;
			throughput /= 1.0 - probability;
		}
	}

	vec3 next_direction;
	vec3 brdf_weight;
	if (!evalIndirectCombinedBRDF(rand2(rng_state), shading_normal, geometry_normal,
				view_direction, brdf_material, brdf_type, next_direction, brdf_weight, vec4(0.0))) {
		vec3 recovered_direction;
		if (luminance(brdf_weight) == 0.0 ||
				!recoverBelowHemisphereSample(next_direction, geometry_normal, recovered_direction)) {
			return false;
		}
		next_direction = recovered_direction;
	}

	throughput *= brdf_weight;
	if (brdf_type == DIFFUSE_TYPE) {
		diffuse_bounces++;
	}
	ray_origin = offset_ray_origin(hit_pos, geometry_normal);
	ray_direction = next_direction;
	return true;
}

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	uvec2 image_size = uvec2(imageSize(image));
	if (any(greaterThanEqual(pixel, image_size))) {
		return;
	}

	vec2 pixel_center = vec2(pixel) + vec2(0.5);
	vec2 in_uv = pixel_center / vec2(image_size);
	vec2 device_position = in_uv * 2.0 - 1.0;

	mat4 inv_view = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec4 target = scene_data_block.data.inv_projection_matrix * vec4(device_position, 1.0, 1.0);
	vec3 primary_origin = (inv_view * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	vec3 primary_direction = (inv_view * vec4(normalize(target.xyz), 0.0)).xyz;

	uint samples_per_pixel = RT_GET_SAMPLE_COUNT();
	uint max_bounces = RT_GET_MAX_BOUNCES();
	uint frame_index = uint(get_rt_param(RT_PARAM_FRAME_INDEX));
	int visualization_mode = int(get_rt_param(RT_PARAM_VIS_MODE));
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));

	// NOTE: the primary ray is identical for every sample (no per-sample
	// jitter), so hoisting the primary trace + material evaluation out of the
	// sample loop looks attractive. Measured on Apple M5 (1080p, 4spp,
	// 2 bounces) it is a wash on the intersector fast path and a ~17%
	// regression on the ray-query path: the cached hit/material state stays
	// live across every secondary traversal and the added ray-trace scratch
	// traffic outweighs the saved primary traversals. Keep the uniform loop.
	vec3 total_radiance = vec3(0.0);

	[[dont_unroll]] for (uint sample_index = 0u; sample_index < samples_per_pixel; sample_index++) {
		vec3 radiance = vec3(0.0);
		vec3 throughput = vec3(1.0);
		uint rng_state = init_rng(pixel, frame_index, sample_index);
		uint diffuse_bounces = 0u;
		vec3 ray_origin = primary_origin;
		vec3 ray_direction = primary_direction;

		[[dont_unroll]] for (uint bounce = 0u; bounce <= max_bounces; bounce++) {
			ComputeHit hit;
			if (!trace_material(ray_origin, ray_direction, 10000.0, hit)) {
				if (visualization_mode == 23 || visualization_mode == 24 || visualization_mode == 25 || visualization_mode == 26) {
					break;
				}
				if (sample_index == 0u && bounce == 0u) {
					imageStore(rt_depth_image, ivec2(pixel), vec4(0.0));
					vec3 far_world = ray_origin + ray_direction * 10000.0;
					vec2 current_uv = project_uv(far_world, curr_vp_unjittered);
					vec2 previous_uv = project_uv(far_world, prev_vp_unjittered);
					imageStore(rt_velocity_image, ivec2(pixel), vec4(previous_uv - current_uv, 0.0, 0.0));
				}
				vec3 sky_color = sample_environment(ray_direction);
#ifdef DENOISER_GUIDES_ENABLED
				if (sample_index == 0u && bounce == 0u) {
					imageStore(denoiser_diffuse_albedo, ivec2(pixel), vec4(sky_color, 1.0));
					imageStore(denoiser_specular_albedo, ivec2(pixel), vec4(0.0));
					imageStore(denoiser_normal_roughness, ivec2(pixel), vec4(-ray_direction, 0.0));
					imageStore(denoiser_roughness, ivec2(pixel), vec4(0.0));
					imageStore(denoiser_specular_hit_dist, ivec2(pixel), vec4(-1.0));
				}
#endif
				radiance += throughput * sky_color;
				break;
			}

			ComputeHitData hit_data = compute_hit_data(hit,
					current_object_to_world(hit.geometry_idx),
					current_world_to_object(hit.geometry_idx),
					ray_origin, ray_direction);
			if (sample_index == 0u && bounce == 0u) {
				write_primary_hit_outputs(pixel, hit, hit_data);
			}
			if (visualization_mode == 23) {
				uint encoded_id = pcg_hash(hit.geometry_idx + 1u);
				radiance = vec3(0.2) + vec3(float(encoded_id & 0xFFu), float((encoded_id >> 8u) & 0xFFu), float((encoded_id >> 16u) & 0xFFu)) * (0.8 / 255.0);
				break;
			} else if (visualization_mode == 24) {
				uint encoded_id = pcg_hash(hit.primitive_idx + 1u);
				radiance = vec3(0.2) + vec3(float(encoded_id & 0xFFu), float((encoded_id >> 8u) & 0xFFu), float((encoded_id >> 16u) & 0xFFu)) * (0.8 / 255.0);
				break;
			} else if (visualization_mode == 25) {
				uint encoded_id = pcg_hash(materials[hit.geometry_idx].material_id + 1u);
				radiance = vec3(0.2) + vec3(float(encoded_id & 0xFFu), float((encoded_id >> 8u) & 0xFFu), float((encoded_id >> 16u) & 0xFFu)) * (0.8 / 255.0);
				break;
			} else if (visualization_mode == 26) {
				uint encoded_id = pcg_hash(hit.geometry_idx * 131u + hit.hit_kind + 1u);
				vec3 identity = vec3(
										float(encoded_id & 0xFFu),
										float((encoded_id >> 8u) & 0xFFu),
										float((encoded_id >> 16u) & 0xFFu)) /
						255.0;
				radiance = hit.procedural ? mix(vec3(0.75, 0.08, 0.08), identity, 0.35) : mix(vec3(0.08, 0.18, 0.75), identity, 0.35);
				break;
			}

			MaterialResult material = evaluate_material(hit, hit_data,
					current_object_to_world(hit.geometry_idx),
					current_world_to_object(hit.geometry_idx),
					ray_direction);
			vec3 view_direction = -ray_direction;
			vec3 shading_normal = clampShadingNormal(material.normal, hit_data.geometry_normal,
					view_direction, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
			radiance += throughput * material.emissive;

			MaterialProperties brdf_material;
			brdf_material.baseColor = material.albedo;
			brdf_material.metalness = material.metalness;
			brdf_material.roughness = material.roughness;
			brdf_material.dielectricF0 = 0.16 * material.specular * material.specular;
			brdf_material.emissive = material.emissive;
			brdf_material.transmissivness = 0.0;
			brdf_material.opacity = 1.0;

#ifdef DENOISER_GUIDES_ENABLED
			if (sample_index == 0u && bounce == 0u) {
				float NdotV = max(dot(shading_normal, view_direction), 0.0001);
				vec3 diffuse_albedo = DLSSRR_computeDiffuseAlbedo(material.albedo, material.metalness);
				vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(material.albedo, material.metalness,
						brdf_material.dielectricF0, material.roughness, NdotV);
				imageStore(denoiser_diffuse_albedo, ivec2(pixel), vec4(diffuse_albedo, 1.0));
				imageStore(denoiser_specular_albedo, ivec2(pixel), vec4(clamp(specular_albedo, vec3(0.0), vec3(1.0)), 1.0));
				imageStore(denoiser_normal_roughness, ivec2(pixel), vec4(shading_normal, material.roughness));
				imageStore(denoiser_roughness, ivec2(pixel), vec4(material.roughness));

				float specular_hit_distance = -1.0;
				if (material.roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
					ComputeHit specular_hit;
					vec3 specular_direction = reflect(-view_direction, shading_normal);
					if (trace_material(offset_ray_origin(hit_data.hit_pos, shading_normal), specular_direction, 10000.0, specular_hit)) {
						specular_hit_distance = specular_hit.t;
					}
				}
				imageStore(denoiser_specular_hit_dist, ivec2(pixel), vec4(specular_hit_distance));
			}
#endif

			if (light_count > 0u) {
				vec3 light_origin = offset_ray_origin(hit_data.hit_pos, hit_data.geometry_normal);
				vec3 direct = lights_evaluate_direct_lighting(light_origin, shading_normal, view_direction,
						brdf_material, rng_state, diffuse_bounces > 0u, light_count);
				radiance += throughput * direct;
			}

			if (bounce >= max_bounces || diffuse_bounces >= MAX_DIFFUSE_BOUNCES) {
				break;
			}

			if (!scatter_from_hit(hit_data.hit_pos, hit_data.geometry_normal, shading_normal, view_direction,
						brdf_material, rng_state, throughput, diffuse_bounces, ray_origin, ray_direction)) {
				break;
			}
		}

		total_radiance += radiance;
	}

	imageStore(image, ivec2(pixel), vec4(total_radiance / float(samples_per_pixel), 1.0));
}
