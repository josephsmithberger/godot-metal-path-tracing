/**************************************************************************/
/*  test_metal_rt_pathtracer_launch.cpp                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_metal_rt_pathtracer_launch)

#include "modules/modules_enabled.gen.h" // For glslang.

#if defined(METAL_ENABLED) && defined(MODULE_GLSLANG_ENABLED)

#include "core/io/image.h"
#include "core/os/os.h"
#include "drivers/metal/metal_objects_shared.h"
#include "drivers/metal/metal_rt_shader_lowering.h"

#include "modules/glslang/shader_compile.h"

namespace TestMetalRTPathtracerLaunch {

// Chunk C10: launch a controlled path-traced scene through the compute-lane
// raytracing pipeline. The kernel below re-expresses the control flow of the
// fork's `scene_raytracing_raygen.glsl` per the C7 strategy: the raygen loop
// becomes the compute kernel body, `traceRayEXT` becomes a ray query, and the
// miss/hit logic is inlined at the call site. The RT-pipeline payload
// (`PathPayload`) does not survive re-expression by design: it exists to cross
// Vulkan stage boundaries, which the compute lane eliminates; the unpacked
// `PathState` working set stays in registers instead.
//
// The image is compared numerically against a CPU reference implementing the
// identical shading math (testing-standard.md, image regression policy). The
// scene is built so that every hit/miss/shadow classification has a wide
// geometric margin from pixel sample positions, which keeps the classification
// deterministic across CPU/GPU floating-point differences; the remaining
// smooth-term differences are covered by the comparison tolerance.

constexpr uint32_t IMAGE_WIDTH = 8;
constexpr uint32_t IMAGE_HEIGHT = 8;
constexpr uint32_t SAMPLES_PER_PIXEL = 2;
constexpr uint32_t MAX_BOUNCES = 1; // Primary hit + one diffuse bounce.
constexpr float MAX_CHANNEL_DIFF = 0.02f;

constexpr uint32_t FLOOR_USER_ID = 7;
constexpr uint32_t BLOCKER_USER_ID = 3;
// Bounce rays exclude the blocker so every bounce deterministically reaches
// the sky regardless of the sampled direction (see kernel comments).
constexpr uint32_t FLOOR_MASK = 0x0F;
constexpr uint32_t BLOCKER_MASK = 0xF0;
constexpr uint32_t PRIMARY_RAY_MASK = 0xFF;
constexpr uint32_t SHADOW_RAY_MASK = 0xFF;
[[maybe_unused]] constexpr uint32_t BOUNCE_RAY_MASK = 0x0F;

struct SceneParams {
	float light_pos_intensity[4] = { 4.0f, 6.0f, 0.0f, 60.0f };
	float sky_radiance[4] = { 0.20f, 0.28f, 0.40f, 0.0f };
	uint32_t dims_spp_bounces[4] = { IMAGE_WIDTH, IMAGE_HEIGHT, SAMPLES_PER_PIXEL, MAX_BOUNCES };
	// Indexed by the instance user ID read back from the ray query. If the
	// user-ID plumbing regressed to instance indices (0 and 1), the magenta
	// canary rows would flood the image and fail the comparison.
	float albedo_table[8][4] = {
		{ 1.0f, 0.0f, 1.0f, 0.0f },
		{ 1.0f, 0.0f, 1.0f, 0.0f },
		{ 1.0f, 0.0f, 1.0f, 0.0f },
		{ 0.85f, 0.15f, 0.10f, 0.0f }, // BLOCKER_USER_ID
		{ 1.0f, 0.0f, 1.0f, 0.0f },
		{ 1.0f, 0.0f, 1.0f, 0.0f },
		{ 1.0f, 0.0f, 1.0f, 0.0f },
		{ 0.55f, 0.45f, 0.25f, 0.0f }, // FLOOR_USER_ID
	};
};

// The re-expressed path-tracer kernel. RNG, ray-origin offsetting, and the
// bounce-loop shape mirror servers/rendering/renderer_rd/shaders/raytracing/
// raytracing_inc.glsl and scene_raytracing_raygen.glsl.
static const char *PATHTRACER_COMPUTE_GLSL = R"GLSL(
#version 460
#extension GL_EXT_ray_query : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std140) uniform SceneParams {
	vec4 light_pos_intensity; // xyz = position, w = intensity
	vec4 sky_radiance;
	uvec4 dims_spp_bounces; // x = width, y = height, z = spp, w = max bounces
	vec4 albedo_table[8];
} params;

layout(set = 0, binding = 2, std430) buffer OutputImage {
	vec4 pixels[];
} out_image;

const float PI = 3.141592653589;

// PCG RNG, identical to raytracing_inc.glsl.
uint pcg_hash(uint seed) {
	uint state = seed * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint init_rng(uvec2 pixel, uint frame, uint sample_idx) {
	uint seed = pixel.x + pixel.y * 65536u + frame * 1000000u + sample_idx * 100000000u;
	return pcg_hash(seed);
}

float rand(inout uint state) {
	state = pcg_hash(state);
	return float(state) / 4294967296.0;
}

// Wachter-Binder ray-origin offset, identical to raytracing_inc.glsl.
float _offset_component(float p, float n_comp, int of_comp) {
	const float origin = 1.0 / 32.0;
	const float float_scale = 1.0 / 65536.0;
	int shifted = floatBitsToInt(p) + ((p >= 0.0) ? of_comp : -of_comp);
	float p_i = intBitsToFloat(shifted);
	return (abs(p) < origin) ? (p + float_scale * n_comp) : p_i;
}

vec3 offset_ray_origin(vec3 p, vec3 n) {
	const float int_scale = 256.0;
	ivec3 of = ivec3(int_scale * n);
	return vec3(
			_offset_component(p.x, n.x, of.x),
			_offset_component(p.y, n.y, of.y),
			_offset_component(p.z, n.z, of.z));
}

// Committed-triangle trace; returns hit distance in r_t and the instance user
// ID (Godot's instance custom index) in r_user_id.
bool trace_closest(vec3 origin, vec3 dir, float t_max, uint mask, out float r_t, out uint r_user_id) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, mask, origin, 0.001, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
	}
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		r_t = rayQueryGetIntersectionTEXT(rq, true);
		r_user_id = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
		return true;
	}
	r_t = -1.0;
	r_user_id = 0u;
	return false;
}

bool trace_occluded(vec3 origin, vec3 dir, float t_max, uint mask) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, mask, origin, 0.0, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (pixel.x >= params.dims_spp_bounces.x || pixel.y >= params.dims_spp_bounces.y) {
		return;
	}

	// Orthographic camera straight down; pixel centers span [-2, 2].
	float px = -2.0 + (float(pixel.x) + 0.5) * (4.0 / float(params.dims_spp_bounces.x));
	float pz = -2.0 + (float(pixel.y) + 0.5) * (4.0 / float(params.dims_spp_bounces.y));

	const uint samples_per_pixel = max(params.dims_spp_bounces.z, 1u);
	const uint max_bounces = params.dims_spp_bounces.w;

	vec3 total_radiance = vec3(0.0);

	for (uint sample_idx = 0u; sample_idx < samples_per_pixel; sample_idx++) {
		// PathState working set (raytracing_inc.glsl), unpacked: the payload
		// packing exists to cross RT-pipeline stage boundaries, which this
		// re-expression eliminates.
		vec3 radiance = vec3(0.0);
		vec3 throughput = vec3(1.0);
		uint rng_state = init_rng(pixel, 0u, sample_idx);

		vec3 ray_origin = vec3(px, 5.0, pz);
		vec3 ray_dir = vec3(0.0, -1.0, 0.0);
		uint ray_mask = 0xFFu;

		for (uint bounce = 0u; bounce <= max_bounces; bounce++) {
			float hit_t;
			uint user_id;
			if (!trace_closest(ray_origin, ray_dir, 100.0, ray_mask, hit_t, user_id)) {
				// Miss: inlined miss-stage logic. Bounce rays reach the sky by
				// construction; primary rays always hit the floor or blocker.
				radiance += throughput * params.sky_radiance.rgb;
				break;
			}

			// Closest-hit logic, inlined. Every shadeable face in this scene
			// is up-facing, so the geometric normal is constant.
			vec3 hit_pos = ray_origin + ray_dir * hit_t;
			vec3 normal = vec3(0.0, 1.0, 0.0);
			vec3 albedo = params.albedo_table[user_id & 7u].rgb;

			// Next-event estimation with an occlusion ray query, mirroring the
			// DLSS-RR precedent in raytracing_closest_hit_common_inc.glsl.
			vec3 to_light = params.light_pos_intensity.xyz - hit_pos;
			float dist2 = dot(to_light, to_light);
			float dist = sqrt(dist2);
			vec3 light_dir = to_light / dist;
			float ndotl = max(dot(normal, light_dir), 0.0);
			if (ndotl > 0.0) {
				vec3 shadow_origin = offset_ray_origin(hit_pos, normal);
				if (!trace_occluded(shadow_origin, light_dir, dist - 0.01, 0xFFu)) {
					radiance += throughput * albedo * (1.0 / PI) * ndotl * params.light_pos_intensity.w / dist2;
				}
			}

			// Cosine-weighted diffuse bounce; the pdf cancels the BRDF.
			throughput *= albedo;
			float u1 = rand(rng_state);
			float u2 = rand(rng_state);
			float r = sqrt(u1);
			float phi = 2.0 * PI * u2;
			ray_dir = vec3(r * cos(phi), sqrt(max(0.0, 1.0 - u1)), r * sin(phi));
			ray_origin = offset_ray_origin(hit_pos, normal);
			// Bounce rays exclude the blocker so the sample direction cannot
			// change hit classification; every bounce reaches the sky.
			ray_mask = 0x0Fu;
		}

		total_radiance += radiance;
	}

	vec3 final_radiance = total_radiance / float(samples_per_pixel);
	out_image.pixels[pixel.y * params.dims_spp_bounces.x + pixel.x] = vec4(final_radiance, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// CPU reference (identical shading math; independent intersection code).
// ---------------------------------------------------------------------------

struct Vec3 {
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

static Vec3 v3(float p_x, float p_y, float p_z) {
	return { p_x, p_y, p_z };
}

static Vec3 sub(Vec3 a, Vec3 b) {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static Vec3 cross3(Vec3 a, Vec3 b) {
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

static float dot3(Vec3 a, Vec3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

struct CPUTriangle {
	Vec3 a, b, c;
};

struct CPUInstance {
	const CPUTriangle *triangles = nullptr;
	uint32_t triangle_count = 0;
	uint32_t mask = 0;
	uint32_t user_id = 0;
};

// Möller-Trumbore; deliberately not the GPU algorithm. Classification margins
// in the scene make both agree; smooth terms are covered by the tolerance.
static bool intersect_triangle(const CPUTriangle &p_tri, Vec3 p_origin, Vec3 p_dir, float p_t_min, float p_t_max, float &r_t) {
	const float epsilon = 1e-7f;
	Vec3 e1 = sub(p_tri.b, p_tri.a);
	Vec3 e2 = sub(p_tri.c, p_tri.a);
	Vec3 pv = cross3(p_dir, e2);
	float det = dot3(e1, pv);
	if (Math::abs(det) < epsilon) {
		return false;
	}
	float inv_det = 1.0f / det;
	Vec3 tv = sub(p_origin, p_tri.a);
	float u = dot3(tv, pv) * inv_det;
	if (u < 0.0f || u > 1.0f) {
		return false;
	}
	Vec3 qv = cross3(tv, e1);
	float v = dot3(p_dir, qv) * inv_det;
	if (v < 0.0f || u + v > 1.0f) {
		return false;
	}
	float t = dot3(e2, qv) * inv_det;
	if (t < p_t_min || t > p_t_max) {
		return false;
	}
	r_t = t;
	return true;
}

static bool cpu_trace_closest(const LocalVector<CPUInstance> &p_instances, Vec3 p_origin, Vec3 p_dir, float p_t_min, float p_t_max, uint32_t p_mask, float &r_t, uint32_t &r_user_id) {
	bool hit = false;
	float best_t = p_t_max;
	for (const CPUInstance &instance : p_instances) {
		if ((instance.mask & p_mask) == 0) {
			continue;
		}
		for (uint32_t i = 0; i < instance.triangle_count; i++) {
			float t;
			if (intersect_triangle(instance.triangles[i], p_origin, p_dir, p_t_min, best_t, t)) {
				best_t = t;
				r_user_id = instance.user_id;
				hit = true;
			}
		}
	}
	r_t = best_t;
	return hit;
}

static float cpu_offset_component(float p, float n_comp, int32_t of_comp) {
	const float origin = 1.0f / 32.0f;
	const float float_scale = 1.0f / 65536.0f;
	int32_t bits;
	memcpy(&bits, &p, sizeof(bits));
	bits += (p >= 0.0f) ? of_comp : -of_comp;
	float p_i;
	memcpy(&p_i, &bits, sizeof(p_i));
	return (Math::abs(p) < origin) ? (p + float_scale * n_comp) : p_i;
}

static Vec3 cpu_offset_ray_origin(Vec3 p, Vec3 n) {
	const float int_scale = 256.0f;
	return v3(
			cpu_offset_component(p.x, n.x, int32_t(int_scale * n.x)),
			cpu_offset_component(p.y, n.y, int32_t(int_scale * n.y)),
			cpu_offset_component(p.z, n.z, int32_t(int_scale * n.z)));
}

static void cpu_render_reference(const SceneParams &p_params, const LocalVector<CPUInstance> &p_instances, LocalVector<float> &r_pixels) {
	const float pi = 3.141592653589f;
	r_pixels.resize(IMAGE_WIDTH * IMAGE_HEIGHT * 4);
	for (uint32_t y = 0; y < IMAGE_HEIGHT; y++) {
		for (uint32_t x = 0; x < IMAGE_WIDTH; x++) {
			float px = -2.0f + (float(x) + 0.5f) * (4.0f / float(IMAGE_WIDTH));
			float pz = -2.0f + (float(y) + 0.5f) * (4.0f / float(IMAGE_HEIGHT));

			float total[3] = { 0.0f, 0.0f, 0.0f };
			for (uint32_t sample_idx = 0; sample_idx < SAMPLES_PER_PIXEL; sample_idx++) {
				float radiance[3] = { 0.0f, 0.0f, 0.0f };
				float throughput[3] = { 1.0f, 1.0f, 1.0f };
				// The RNG only drives the bounce direction, which is
				// unobservable by construction (bounce rays always reach the
				// sky), so the reference does not consume it.

				Vec3 ray_origin = v3(px, 5.0f, pz);
				Vec3 ray_dir = v3(0.0f, -1.0f, 0.0f);

				float hit_t;
				uint32_t user_id;
				if (!cpu_trace_closest(p_instances, ray_origin, ray_dir, 0.001f, 100.0f, PRIMARY_RAY_MASK, hit_t, user_id)) {
					// Never reached in this scene: every primary ray hits the
					// floor or the blocker interior.
					for (int c = 0; c < 3; c++) {
						radiance[c] += throughput[c] * p_params.sky_radiance[c];
					}
				} else {
					Vec3 hit_pos = v3(ray_origin.x + ray_dir.x * hit_t, ray_origin.y + ray_dir.y * hit_t, ray_origin.z + ray_dir.z * hit_t);
					Vec3 normal = v3(0.0f, 1.0f, 0.0f);
					const float *albedo = p_params.albedo_table[user_id & 7u];

					Vec3 to_light = sub(v3(p_params.light_pos_intensity[0], p_params.light_pos_intensity[1], p_params.light_pos_intensity[2]), hit_pos);
					float dist2 = dot3(to_light, to_light);
					float dist = Math::sqrt(dist2);
					Vec3 light_dir = v3(to_light.x / dist, to_light.y / dist, to_light.z / dist);
					float ndotl = MAX(dot3(normal, light_dir), 0.0f);
					if (ndotl > 0.0f) {
						Vec3 shadow_origin = cpu_offset_ray_origin(hit_pos, normal);
						float shadow_t;
						uint32_t shadow_id;
						bool occluded = cpu_trace_closest(p_instances, shadow_origin, light_dir, 0.0f, dist - 0.01f, SHADOW_RAY_MASK, shadow_t, shadow_id);
						if (!occluded) {
							for (int c = 0; c < 3; c++) {
								radiance[c] += throughput[c] * albedo[c] * (1.0f / pi) * ndotl * p_params.light_pos_intensity[3] / dist2;
							}
						}
					}

					// The diffuse bounce is direction-independent by
					// construction: bounce rays exclude the blocker
					// (BOUNCE_RAY_MASK) and every shaded face points up, so the
					// sampled hemisphere direction always reaches the sky. The
					// cosine pdf cancels the BRDF, leaving albedo * sky.
					if (MAX_BOUNCES >= 1) {
						for (int c = 0; c < 3; c++) {
							throughput[c] *= albedo[c];
							radiance[c] += throughput[c] * p_params.sky_radiance[c];
						}
					}
				}

				for (int c = 0; c < 3; c++) {
					total[c] += radiance[c];
				}
			}

			float *out = &r_pixels[(y * IMAGE_WIDTH + x) * 4];
			for (int c = 0; c < 3; c++) {
				out[c] = total[c] / float(SAMPLES_PER_PIXEL);
			}
			out[3] = 1.0f;
		}
	}
}

// ---------------------------------------------------------------------------
// CPU pipeline-mapping cases.
// ---------------------------------------------------------------------------

struct ComputeLaneGroups {
	LocalVector<RDD::PipelineShader> shaders;
	LocalVector<uint32_t> raygen_indices;
	LocalVector<uint32_t> miss_indices;
	LocalVector<RDD::HitGroup> hit_groups;

	ComputeLaneGroups() {
		RDD::PipelineShader kernel;
		kernel.shader = RDD::ShaderID(uint64_t(1));
		kernel.shader_stage = RDD::SHADER_STAGE_COMPUTE;
		shaders.push_back(kernel);
		raygen_indices.push_back(0);
		// Hit groups on the compute lane are stable sentinel records only.
		hit_groups.push_back(RDD::HitGroup());
		hit_groups.push_back(RDD::HitGroup());
	}
};

TEST_CASE("[MetalRT] C10 maps a compute-lane ray-generation group") {
	ComputeLaneGroups input;
	MDRaytracingPipeline pipeline;
	String error;
	REQUIRE_MESSAGE(pipeline.configure_shader_groups(input.shaders, input.raygen_indices, input.miss_indices, input.hit_groups, 2, &error), error);

	CHECK(pipeline.uses_compute_lane);
	CHECK(pipeline.raygen_group_count == 1);
	CHECK(pipeline.miss_group_count == 0);
	CHECK(pipeline.hit_group_count == 2);
	REQUIRE(pipeline.shader_groups.size() == 3);
	CHECK(pipeline.shader_groups[0].type == MDRaytracingPipeline::ShaderGroupType::RAYGEN);
	CHECK(pipeline.shader_groups[1].type == MDRaytracingPipeline::ShaderGroupType::EMPTY_HIT);
	CHECK(pipeline.shader_groups[2].type == MDRaytracingPipeline::ShaderGroupType::EMPTY_HIT);

	// Compatibility SBT records stay byte-stable on the compute lane.
	uint8_t bytes[MDRaytracingPipeline::SHADER_GROUP_HANDLE_SIZE * 2];
	const uint32_t hit_indices[] = { 0, 1 };
	REQUIRE_MESSAGE(pipeline.get_shader_group_handles(1, VectorView<uint32_t>(hit_indices, 2), bytes, MDRaytracingPipeline::SHADER_GROUP_HANDLE_SIZE, &error), error);
	const auto *first = reinterpret_cast<const MDRaytracingPipeline::ShaderGroupHandle *>(bytes);
	CHECK(first->magic == MDRaytracingPipeline::ShaderGroupHandle::MAGIC);
	CHECK(first->group_index == 1);
	CHECK(first->type == MDRaytracingPipeline::ShaderGroupType::EMPTY_HIT);
}

TEST_CASE("[MetalRT] C10 rejects invalid compute-lane configurations") {
	ComputeLaneGroups input;
	MDRaytracingPipeline pipeline;
	String error;

	// Two compute raygens: the re-expressed kernel is monolithic.
	{
		ComputeLaneGroups two = input;
		RDD::PipelineShader second;
		second.shader = RDD::ShaderID(uint64_t(2));
		second.shader_stage = RDD::SHADER_STAGE_COMPUTE;
		two.shaders.push_back(second);
		two.raygen_indices.push_back(1);
		CHECK_FALSE(pipeline.configure_shader_groups(two.shaders, two.raygen_indices, two.miss_indices, two.hit_groups, 2, &error));
		CHECK(error.contains("exactly one"));
	}

	// Miss shaders are inlined into the kernel.
	{
		ComputeLaneGroups with_miss = input;
		RDD::PipelineShader miss;
		miss.shader = RDD::ShaderID(uint64_t(2));
		miss.shader_stage = RDD::SHADER_STAGE_MISS;
		with_miss.shaders.push_back(miss);
		with_miss.miss_indices.push_back(1);
		CHECK_FALSE(pipeline.configure_shader_groups(with_miss.shaders, with_miss.raygen_indices, with_miss.miss_indices, with_miss.hit_groups, 2, &error));
		CHECK(error.contains("miss"));
	}

	// Hit groups must be empty sentinels; hit logic is inlined.
	{
		ComputeLaneGroups with_hit = input;
		RDD::PipelineShader closest_hit;
		closest_hit.shader = RDD::ShaderID(uint64_t(2));
		closest_hit.shader_stage = RDD::SHADER_STAGE_CLOSEST_HIT;
		with_hit.shaders.push_back(closest_hit);
		with_hit.hit_groups[0].closest_hit_shader_index = 1;
		CHECK_FALSE(pipeline.configure_shader_groups(with_hit.shaders, with_hit.raygen_indices, with_hit.miss_indices, with_hit.hit_groups, 2, &error));
		CHECK(error.contains("inlines hit logic"));
	}
}

// ---------------------------------------------------------------------------
// GPU controlled-scene launch.
// ---------------------------------------------------------------------------

struct SceneBLAS {
	NS::SharedPtr<MTL::Buffer> vertex_buffer;
	NS::SharedPtr<MTL::Buffer> scratch;
	NS::SharedPtr<MTL::AccelerationStructure> accel;
	MDAccelerationStructure *structure = nullptr;

	~SceneBLAS() {
		if (structure) {
			memdelete(structure);
		}
	}

	bool build(MTL::Device *p_device, MTL::CommandQueue *p_queue, const float *p_vertices, uint32_t p_vertex_count) {
		vertex_buffer = NS::TransferPtr(p_device->newBuffer(p_vertices, p_vertex_count * 3 * sizeof(float), MTL::ResourceStorageModeShared));
		if (!vertex_buffer) {
			return false;
		}

		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> geometry = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		geometry->setVertexBuffer(vertex_buffer.get());
		geometry->setVertexStride(sizeof(float) * 3);
		geometry->setTriangleCount(p_vertex_count / 3);
		geometry->setOpaque(true);

		NS::Object *geometry_object = geometry.get();
		NS::SharedPtr<NS::Array> geometries = NS::TransferPtr(NS::Array::array(&geometry_object, 1)->retain());
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> blas_descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
		blas_descriptor->setGeometryDescriptors(geometries.get());

		MTL::AccelerationStructureSizes sizes = p_device->accelerationStructureSizes(blas_descriptor.get());
		structure = memnew(MDAccelerationStructure(MDAccelerationStructure::Type::BLAS, blas_descriptor, sizes, {}));
		if (!structure->allocate(p_device)) {
			return false;
		}
		scratch = NS::TransferPtr(p_device->newBuffer(structure->build_scratch_size, MTL::ResourceStorageModePrivate));
		if (!scratch) {
			return false;
		}
		NS::SharedPtr<MTL::CommandBuffer> command = NS::RetainPtr(p_queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> encoder = NS::RetainPtr(command->accelerationStructureCommandEncoder());
		structure->encode_build(encoder.get(), scratch.get());
		encoder->endEncoding();
		command->commit();
		command->waitUntilCompleted();
		if (command->status() != MTL::CommandBufferStatusCompleted) {
			return false;
		}
		accel = structure->accel;
		return true;
	}
};

TEST_CASE_PENDING("[MetalRT][GPU] C10 path-tracer scene launch matches the CPU reference") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}
	if (__builtin_available(macOS 12.0, iOS 15.0, tvOS 16.0, *)) {
		// User-ID instance descriptors are available; they are the C10 floor
		// for shader-visible instance custom indices.
	} else {
		MESSAGE("SKIP_REASON=unsupported_os");
		return;
	}

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);

	// Floor quad: y = 0, x/z in [-3, 3], user ID 7. Primary rays hit its
	// interior for every pixel column that is not covered by the blocker.
	static const CPUTriangle FLOOR_TRIANGLES[] = {
		{ v3(-3, 0, -3), v3(3, 0, -3), v3(3, 0, 3) },
		{ v3(-3, 0, -3), v3(3, 0, 3), v3(-3, 0, 3) },
	};
	// Blocker quad: y = 3, x in [1, 2], z in [-2, 2], user ID 3. It shadows
	// the floor's left half (light at x = 4) and is itself directly visible to
	// the camera in two pixel columns. All classification edges sit at least
	// 0.125 world units away from every projected pixel sample.
	static const CPUTriangle BLOCKER_TRIANGLES[] = {
		{ v3(1, 3, -2), v3(2, 3, -2), v3(2, 3, 2) },
		{ v3(1, 3, -2), v3(2, 3, 2), v3(1, 3, 2) },
	};

	auto flatten = [](const CPUTriangle *p_tris, uint32_t p_count, LocalVector<float> &r_out) {
		r_out.clear();
		for (uint32_t i = 0; i < p_count; i++) {
			const Vec3 verts[3] = { p_tris[i].a, p_tris[i].b, p_tris[i].c };
			for (const Vec3 &v : verts) {
				r_out.push_back(v.x);
				r_out.push_back(v.y);
				r_out.push_back(v.z);
			}
		}
	};

	LocalVector<float> floor_vertices;
	LocalVector<float> blocker_vertices;
	flatten(FLOOR_TRIANGLES, 2, floor_vertices);
	flatten(BLOCKER_TRIANGLES, 2, blocker_vertices);

	SceneBLAS floor_blas;
	SceneBLAS blocker_blas;
	REQUIRE(floor_blas.build(device.get(), queue.get(), floor_vertices.ptr(), 6));
	REQUIRE(blocker_blas.build(device.get(), queue.get(), blocker_vertices.ptr(), 6));

	// Two user-ID instances through the C6 instance writer.
	MDAccelerationStructureInstance instances[2];
	{
		RDD::AccelerationStructureInstance floor_instance;
		floor_instance.id = FLOOR_USER_ID;
		floor_instance.mask = FLOOR_MASK;
		floor_instance.blas = RDD::AccelerationStructureID(floor_blas.structure);
		REQUIRE(instances[0].write(floor_instance));

		RDD::AccelerationStructureInstance blocker_instance;
		blocker_instance.id = BLOCKER_USER_ID;
		blocker_instance.mask = BLOCKER_MASK;
		blocker_instance.blas = RDD::AccelerationStructureID(blocker_blas.structure);
		REQUIRE(instances[1].write(blocker_instance));
	}
	NS::SharedPtr<MTL::Buffer> instance_buffer = NS::TransferPtr(device->newBuffer(instances, sizeof(instances), MTL::ResourceStorageModeShared));
	REQUIRE(instance_buffer);

	NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
	tlas_descriptor->setInstanceCount(2);
	tlas_descriptor->setInstanceDescriptorStride(sizeof(MDAccelerationStructureInstance));
	tlas_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);
	MTL::AccelerationStructureSizes tlas_sizes = device->accelerationStructureSizes(tlas_descriptor.get());
	MDAccelerationStructure tlas(MDAccelerationStructure::Type::TLAS, tlas_descriptor, tlas_sizes, {}, 2);
	REQUIRE(tlas.allocate(device.get()));
	REQUIRE(tlas.prepare_tlas_build(instance_buffer.get(), 0, 2));
	CHECK(tlas.resident_blases.size() == 2);
	NS::SharedPtr<MTL::Buffer> tlas_scratch = NS::TransferPtr(device->newBuffer(tlas.build_scratch_size, MTL::ResourceStorageModePrivate));
	REQUIRE(tlas_scratch);
	{
		NS::SharedPtr<MTL::CommandBuffer> command = NS::RetainPtr(queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> encoder = NS::RetainPtr(command->accelerationStructureCommandEncoder());
		tlas.encode_build(encoder.get(), tlas_scratch.get());
		encoder->endEncoding();
		command->commit();
		command->waitUntilCompleted();
		REQUIRE(command->status() == MTL::CommandBufferStatusCompleted);
	}

	// Re-expressed kernel through the real lane: GLSL -> glslang -> SPIRV-Cross
	// MSL (ray-query floor 2.4, classic bindings) -> Metal runtime compile.
	String glsl_error;
	Vector<uint8_t> spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, String::utf8(PATHTRACER_COMPUTE_GLSL),
			RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_3, RDC::SHADER_SPIRV_VERSION_1_6, &glsl_error);
	REQUIRE_MESSAGE(!spirv.is_empty(), vformat("glslang failed: %s", glsl_error));
	MetalRTShaderLowering::Result lowered = MetalRTShaderLowering::lower_spirv(RDC::SHADER_STAGE_COMPUTE, spirv, 2, 4, false, false);
	REQUIRE_MESSAGE(lowered.ok, vformat("SPIRV-Cross lowering failed: %s", lowered.error));
	CHECK(lowered.msl_source.contains("user_instance_id"));

	NS::Error *ns_error = nullptr;
	NS::SharedPtr<MTL::CompileOptions> compile_options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
	compile_options->setLanguageVersion(MTL::LanguageVersion2_4);
	CharString msl_utf8 = lowered.msl_source.utf8();
	NS::SharedPtr<NS::String> msl_string = NS::TransferPtr(NS::String::alloc()->init(msl_utf8.get_data(), NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::Library> library = NS::TransferPtr(device->newLibrary(msl_string.get(), compile_options.get(), &ns_error));
	if (!library) {
		FAIL(vformat("MSL runtime compile failed: %s", ns_error ? ns_error->localizedDescription()->utf8String() : "unknown error"));
		return;
	}
	CharString entry_utf8 = lowered.entry_point.utf8();
	NS::SharedPtr<NS::String> entry_name = NS::TransferPtr(NS::String::alloc()->init(entry_utf8.get_data(), NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::Function> function = NS::TransferPtr(library->newFunction(entry_name.get()));
	REQUIRE(function);

	// Godot-shaped pipeline: compute-lane raygen plus sentinel hit groups.
	ComputeLaneGroups groups;
	MDRaytracingPipeline pipeline;
	String error;
	REQUIRE_MESSAGE(pipeline.configure_shader_groups(groups.shaders, groups.raygen_indices, groups.miss_indices, groups.hit_groups, 2, &error), error);
	REQUIRE_MESSAGE(pipeline.create_compute_lane(device.get(), function.get(), MTL::Size(8, 8, 1), &error), error);
	REQUIRE(pipeline.is_valid());

	SceneParams params;
	NS::SharedPtr<MTL::Buffer> params_buffer = NS::TransferPtr(device->newBuffer(&params, sizeof(params), MTL::ResourceStorageModeShared));
	NS::SharedPtr<MTL::Buffer> output_buffer = NS::TransferPtr(device->newBuffer(IMAGE_WIDTH * IMAGE_HEIGHT * 4 * sizeof(float), MTL::ResourceStorageModeShared));
	REQUIRE(params_buffer);
	REQUIRE(output_buffer);
	memset(output_buffer->contents(), 0, output_buffer->length());

	{
		NS::SharedPtr<MTL::CommandBuffer> command = NS::RetainPtr(queue->commandBuffer());
		NS::SharedPtr<MTL::ComputeCommandEncoder> encoder = NS::RetainPtr(command->computeCommandEncoder());
		REQUIRE(encoder);
		encoder->setComputePipelineState(pipeline.get_compute_pipeline_state());
		encoder->setAccelerationStructure(tlas.accel.get(), 0);
		encoder->setBuffer(params_buffer.get(), 0, 1);
		encoder->setBuffer(output_buffer.get(), 0, 2);
		// Residency for the TLAS-referenced primitive structures, exactly what
		// the driver's uniform-set bind now provides through resident_blases.
		encoder->useResources(reinterpret_cast<const MTL::Resource *const *>(tlas.resident_blases.ptr()), tlas.resident_blases.size(), MTL::ResourceUsageRead);

		// The same rounded-up grid math as MDCommandBuffer::trace_rays().
		const MTL::Size local = pipeline.get_threads_per_threadgroup();
		MTL::Size dispatch_groups = MTL::Size(
				(IMAGE_WIDTH + local.width - 1) / local.width,
				(IMAGE_HEIGHT + local.height - 1) / local.height,
				1);
		encoder->dispatchThreadgroups(dispatch_groups, local);
		encoder->endEncoding();
		command->commit();
		command->waitUntilCompleted();
		REQUIRE(command->status() == MTL::CommandBufferStatusCompleted);
		CHECK(command->error() == nullptr);
	}

	LocalVector<float> gpu_pixels;
	gpu_pixels.resize(IMAGE_WIDTH * IMAGE_HEIGHT * 4);
	memcpy(gpu_pixels.ptr(), output_buffer->contents(), gpu_pixels.size() * sizeof(float));

	LocalVector<CPUInstance> cpu_instances;
	cpu_instances.push_back(CPUInstance{ FLOOR_TRIANGLES, 2, FLOOR_MASK, FLOOR_USER_ID });
	cpu_instances.push_back(CPUInstance{ BLOCKER_TRIANGLES, 2, BLOCKER_MASK, BLOCKER_USER_ID });
	LocalVector<float> cpu_pixels;
	cpu_render_reference(params, cpu_instances, cpu_pixels);

	// Numeric comparison against the CPU reference.
	float max_diff = 0.0f;
	double diff_accum = 0.0;
	for (uint32_t i = 0; i < gpu_pixels.size(); i++) {
		float diff = Math::abs(gpu_pixels[i] - cpu_pixels[i]);
		max_diff = MAX(max_diff, diff);
		diff_accum += diff;
	}
	float mean_diff = float(diff_accum / gpu_pixels.size());
	CHECK_MESSAGE(max_diff < MAX_CHANNEL_DIFF, vformat("GPU/CPU image mismatch: max channel diff %f", max_diff));

	// Semantic checks: shadowed floor < lit floor; the blocker columns show
	// the blocker albedo; every pixel receives the bounce (sky) term.
	auto luminance_at = [&](uint32_t p_x, uint32_t p_y) {
		const float *px = &gpu_pixels[(p_y * IMAGE_WIDTH + p_x) * 4];
		return 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
	};
	// Column 1 (px = -1.25) is shadowed; column 5 (px = 0.75) is lit floor.
	CHECK(luminance_at(1, 4) < luminance_at(5, 4));
	// Column 6 (px = 1.25) sees the blocker: red channel dominates.
	const float *blocker_pixel = &gpu_pixels[(4 * IMAGE_WIDTH + 6) * 4];
	CHECK(blocker_pixel[0] > blocker_pixel[1]);
	// The shadowed floor still carries the sky bounce, so it is not black.
	CHECK(luminance_at(1, 4) > 0.01f);

	// Image artifacts for the runner (testing-standard.md, L5 evidence).
	String artifact_dir = OS::get_singleton()->get_environment("GODOT_MRT_ARTIFACT_DIR");
	if (!artifact_dir.is_empty()) {
		auto save_image = [&](const LocalVector<float> &p_pixels, const String &p_path) {
			Vector<uint8_t> rgba8;
			rgba8.resize(IMAGE_WIDTH * IMAGE_HEIGHT * 4);
			for (uint32_t i = 0; i < p_pixels.size(); i++) {
				rgba8.write[i] = uint8_t(CLAMP(p_pixels[i], 0.0f, 1.0f) * 255.0f + 0.5f);
			}
			Ref<Image> image = Image::create_from_data(IMAGE_WIDTH, IMAGE_HEIGHT, false, Image::FORMAT_RGBA8, rgba8);
			if (image.is_valid()) {
				image->save_png(p_path);
			}
		};
		save_image(gpu_pixels, artifact_dir.path_join("c10_pathtracer_launch_gpu.png"));
		save_image(cpu_pixels, artifact_dir.path_join("c10_pathtracer_launch_cpu_reference.png"));
	}

	print_line(vformat("MetalRT C10 path-tracer launch: device=\"%s\" image=%dx%d spp=%d bounces=%d instances=2 max_diff=%f mean_diff=%f",
			device->name()->utf8String(), IMAGE_WIDTH, IMAGE_HEIGHT, SAMPLES_PER_PIXEL, MAX_BOUNCES + 1, max_diff, mean_diff));
}

} // namespace TestMetalRTPathtracerLaunch

#endif // METAL_ENABLED && MODULE_GLSLANG_ENABLED
