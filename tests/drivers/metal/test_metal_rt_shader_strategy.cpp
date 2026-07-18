/**************************************************************************/
/*  test_metal_rt_shader_strategy.cpp                                     */
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

TEST_FORCE_LINK(test_metal_rt_shader_strategy)

#include "modules/modules_enabled.gen.h" // For glslang.

#if defined(METAL_ENABLED) && defined(MODULE_GLSLANG_ENABLED)

#include "core/io/file_access.h"
#include "drivers/metal/metal_objects_shared.h"
#include "drivers/metal/metal_rt_shader_lowering.h"
#include "drivers/metal/rendering_shader_container_metal.h"
#include "tests/test_utils.h"

#include "modules/glslang/shader_compile.h"

namespace TestMetalRTShaderStrategy {

// Shader-lowering shader-lowering spike. These tests provide the empirical evidence
// behind docs/rt_metal_port/shader_strategy.md: which RT shader forms the
// existing SPIRV-Cross lane can lower to MSL, and that the native
// metal::raytracing::intersector lane executes correctly against the BLAS/TLAS
// acceleration structures.

// Ray-query compute kernel: the shader form the existing SPIRV-Cross lane is
// expected to lower. Two fixed rays; the dispatch decides hit vs. miss.
static const char *RAY_QUERY_COMPUTE_GLSL = R"GLSL(
#version 460
#extension GL_EXT_ray_query : enable

layout(local_size_x = 2, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std430) readonly buffer Rays {
	vec4 origin_tmin[2];
	vec4 direction_tmax[2];
} rays;

layout(set = 0, binding = 2, std430) buffer Results {
	// x = hit (1.0/0.0), y = distance, z = primitive index, w = instance index.
	vec4 hit_t_primitive_instance[2];
} results;

void main() {
	uint idx = gl_LocalInvocationID.x;
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xffu,
			rays.origin_tmin[idx].xyz, rays.origin_tmin[idx].w,
			rays.direction_tmax[idx].xyz, rays.direction_tmax[idx].w);
	while (rayQueryProceedEXT(rq)) {
	}
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		results.hit_t_primitive_instance[idx] = vec4(1.0,
				rayQueryGetIntersectionTEXT(rq, true),
				float(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)),
				float(rayQueryGetIntersectionInstanceIdEXT(rq, true)));
	} else {
		results.hit_t_primitive_instance[idx] = vec4(0.0, -1.0, -1.0, -1.0);
	}
}
)GLSL";

// Minimal ray-generation stage: the RT-pipeline shader form assumption A7
// expects the SPIRV-Cross lane to reject.
static const char *RAYGEN_GLSL = R"GLSL(
#version 460
#extension GL_EXT_ray_tracing : enable

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, rgba32f) uniform image2D output_image;
layout(location = 0) rayPayloadEXT vec4 payload;

void main() {
	payload = vec4(0.0);
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xffu, 0u, 0u, 0u,
			vec3(0.0, -0.25, -2.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0, 0);
	imageStore(output_image, ivec2(gl_LaunchIDEXT.xy), payload);
}
)GLSL";

// Minimal closest-hit stage with an incoming payload and hit attributes.
static const char *CLOSEST_HIT_GLSL = R"GLSL(
#version 460
#extension GL_EXT_ray_tracing : enable

layout(location = 0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec2 barycentrics;

void main() {
	payload = vec4(barycentrics, float(gl_PrimitiveID), gl_HitTEXT);
}
)GLSL";

// Compute-lane facsimile of the material access pattern used by the scene RT
// shaders: a GPU-addressed material record selects an entry from an unbounded
// texture array. This deliberately goes through RenderingShaderContainerMetal,
// not just the small SHADER_LOWERING lowering probe.
static const char *BINDLESS_MATERIAL_GLSL = R"GLSL(
#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_gpu_shader_int64 : require

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(buffer_reference, std430) readonly buffer Material {
	uint texture_index;
};

layout(set = 0, binding = 0, std430) readonly buffer MaterialAddresses {
	uint64_t material_address;
} material_addresses;
layout(set = 0, binding = 1) uniform sampler material_sampler;
layout(set = 0, binding = 2, std430) writeonly buffer Result {
	vec4 color;
} result;
layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

void main() {
	Material material = Material(material_addresses.material_address);
	result.color = texture(sampler2D(bindless_textures[nonuniformEXT(material.texture_index)], material_sampler), vec2(0.5));
}
)GLSL";

// Native MSL twin of RAY_QUERY_COMPUTE_GLSL, written directly against
// metal::raytracing::intersector at the MSL 2.3 (macOS 11) floor.
static const char *NATIVE_INTERSECTOR_MSL = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>

using namespace metal;
using namespace metal::raytracing;

struct SpikeRays {
	float4 origin_tmin[2];
	float4 direction_tmax[2];
};

struct SpikeResults {
	float4 hit_t_primitive_instance[2];
};

kernel void trace_spike(
		instance_acceleration_structure tlas [[buffer(0)]],
		const device SpikeRays &rays [[buffer(1)]],
		device SpikeResults &results [[buffer(2)]],
		uint tid [[thread_position_in_grid]]) {
	ray r;
	r.origin = rays.origin_tmin[tid].xyz;
	r.min_distance = rays.origin_tmin[tid].w;
	r.direction = rays.direction_tmax[tid].xyz;
	r.max_distance = rays.direction_tmax[tid].w;

	intersector<instancing, triangle_data> isect;
	isect.assume_geometry_type(geometry_type::triangle);
	intersection_result<instancing, triangle_data> hit = isect.intersect(r, tlas, 0xffu);

	if (hit.type == intersection_type::triangle) {
		results.hit_t_primitive_instance[tid] = float4(1.0f, hit.distance, float(hit.primitive_id), float(hit.instance_id));
	} else {
		results.hit_t_primitive_instance[tid] = float4(0.0f, -1.0f, -1.0f, -1.0f);
	}
}
)MSL";

// Reduced standalone SPIRV-Cross-shaped scene kernel for the production INTERSECTOR
// rewrite. The test specializes RT_FLAGS to 0 and ALL_OPAQUE from this same
// patched library, proving the query and injected intersector bodies against
// identical rays and acceleration structures.
static const char *INTERSECTOR_PATCH_PARITY_MSL = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>

using namespace metal;
using namespace metal::raytracing;

struct ComputeHit {
	float t;
	uint geometry_idx;
	uint primitive_idx;
	float2 barycentrics;
	short front_face;
	short procedural;
	uint hit_kind;
};

struct INTERSECTORRays {
	float4 origin_tmin[4];
	float4 direction_tmax[4];
};

struct INTERSECTORResults {
	float4 material_shadow[4];
};

constant uint RT_FLAGS_tmp [[function_constant(0)]];
constant uint RT_FLAGS = is_function_constant_defined(RT_FLAGS_tmp) ? RT_FLAGS_tmp : 0u;

static inline __attribute__((always_inline))
bool trace_material(thread const float3& origin, thread const float3& direction, thread const float& max_distance, thread ComputeHit& hit, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
	raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data> query;
	raytracing::intersection_params params;
	params.force_opacity(raytracing::forced_opacity::opaque);
	params.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
	query.reset(raytracing::ray(origin, direction, 0.001, max_distance), tlas, 0xFFu, params);
	while (query.next())
	{
		query.commit_triangle_intersection();
	}
	if (query.get_committed_intersection_type() != raytracing::intersection_type::triangle)
	{
		return false;
	}
	hit.t = query.get_committed_distance();
	hit.geometry_idx = uint(query.get_committed_user_instance_id());
	hit.primitive_idx = uint(query.get_committed_primitive_id());
	hit.barycentrics = query.get_committed_triangle_barycentric_coord();
	hit.front_face = short(query.is_committed_triangle_front_facing());
	hit.procedural = short(false);
	hit.hit_kind = bool(hit.front_face) ? 254u : 255u;
	return true;
}

static inline __attribute__((always_inline))
bool trace_shadow_blocked(thread const float3& origin, thread const float3& direction, thread const float& max_distance, const raytracing::acceleration_structure<raytracing::instancing> tlas)
{
	raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data> query;
	raytracing::intersection_params params;
	params.force_opacity(raytracing::forced_opacity::opaque);
	params.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
	params.accept_any_intersection(true);
	query.reset(raytracing::ray(origin, direction, 0.001, max_distance), tlas, 0xFFu, params);
	while (query.next())
	{
		query.commit_triangle_intersection();
	}
	return query.get_committed_intersection_type() == raytracing::intersection_type::triangle;
}

kernel void intersector_trace_parity(
		const raytracing::instance_acceleration_structure tlas [[buffer(0)]],
		const device INTERSECTORRays& rays [[buffer(1)]],
		device INTERSECTORResults& results [[buffer(2)]],
		uint tid [[thread_position_in_grid]])
{
	ComputeHit hit = {};
	float3 origin = rays.origin_tmin[tid].xyz;
	float3 direction = rays.direction_tmax[tid].xyz;
	float max_distance = rays.direction_tmax[tid].w;
	bool material_hit = trace_material(origin, direction, max_distance, hit, tlas);
	bool shadow_hit = trace_shadow_blocked(origin, direction, max_distance, tlas);
	results.material_shadow[tid] = float4(material_hit ? 1.0 : 0.0,
		material_hit ? hit.t : -1.0, shadow_hit ? 1.0 : 0.0,
		material_hit ? float(hit.geometry_idx) : -1.0);
}
)MSL";

struct SpikeRayData {
	float origin_tmin[2][4] = {
		// Aimed at the triangle from z = -2 along +z; expected hit at t = 2.
		{ 0.0f, -0.25f, -2.0f, 0.001f },
		// Same origin aimed away along -z; expected miss.
		{ 0.0f, -0.25f, -2.0f, 0.001f },
	};
	float direction_tmax[2][4] = {
		{ 0.0f, 0.0f, 1.0f, 100.0f },
		{ 0.0f, 0.0f, -1.0f, 100.0f },
	};
};

struct SpikeResultData {
	float hit_t_primitive_instance[2][4] = {};
};

struct INTERSECTORRayData {
	float origin_tmin[4][4] = {
		{ 0.0f, -0.25f, -2.0f, 0.001f },
		{ 0.0f, -0.25f, -2.0f, 0.001f },
		{ 0.0f, -0.25f, -2.0f, 0.001f },
		{ 0.0f, -0.25f, -0.0005f, 0.001f },
	};
	float direction_tmax[4][4] = {
		{ 0.0f, 0.0f, 1.0f, 100.0f }, // Hit at t=2.
		{ 0.0f, 0.0f, -1.0f, 100.0f }, // Direction miss.
		{ 0.0f, 0.0f, 1.0f, 1.0f }, // Clipped by t_max.
		{ 0.0f, 0.0f, 1.0f, 100.0f }, // Clipped by fixed t_min=0.001.
	};
};

struct INTERSECTORResultData {
	float material_shadow[4][4] = {};
};

static Vector<uint8_t> compile_stage_to_spirv(RDC::ShaderStage p_stage, const char *p_glsl, String *r_error) {
	return compile_glslang_shader(p_stage, String::utf8(p_glsl),
			RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_3, RDC::SHADER_SPIRV_VERSION_1_6, r_error);
}

static std::string load_msl_rewrite_fixture(const char *p_name) {
	String path = TestUtils::get_data_path(String("metal_rt/") + p_name);
	String source = FileAccess::get_file_as_string(path);
	return std::string(source.utf8().get_data());
}

static bool build_bindless_material_container(const Vector<uint8_t> &p_spirv, Ref<RenderingShaderContainerMetal> &r_container, String &r_msl_source) {
	const MetalDeviceProfile *profile = MetalDeviceProfile::get_profile(
			MetalDeviceProfile::Platform::macOS, MetalDeviceProfile::GPU::Apple8, os_version::MACOS_13_0);
	ERR_FAIL_NULL_V(profile, false);

	r_container.instantiate();
	r_container->set_device_profile(profile);

	Vector<RDC::ShaderStageSPIRVData> stages;
	stages.resize(1);
	stages.write[0].shader_stage = RDC::SHADER_STAGE_COMPUTE;
	stages.write[0].spirv = p_spirv;
	ERR_FAIL_COND_V(!r_container->set_code_from_spirv("bindless_material_probe", stages), false);
	ERR_FAIL_COND_V(r_container->shaders.size() != 1 || r_container->mtl_shaders.size() != 1, false);

	const RenderingShaderContainer::Shader &shader = r_container->shaders[0];
	const RenderingShaderContainerMetal::StageData &stage_data = r_container->mtl_shaders[0];
	Vector<uint8_t> source;
	source.resize(shader.code_decompressed_size);
	ERR_FAIL_COND_V(!r_container->decompress_code(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size(),
							shader.code_compression_flags, source.ptrw(), source.size()),
			false);
	r_msl_source = String::utf8(reinterpret_cast<const char *>(source.ptr()), stage_data.source_size);
	return true;
}

// Builds the BLAS/TLAS single-triangle BLAS + one-instance TLAS scene and leaves
// both structures ready for a compute dispatch.
struct SpikeScene {
	NS::SharedPtr<MTL::Buffer> vertex_buffer;
	NS::SharedPtr<MTL::Buffer> instance_buffer;
	NS::SharedPtr<MTL::AccelerationStructure> blas;
	NS::SharedPtr<MTL::AccelerationStructure> tlas;

	bool build(MTL::Device *p_device, MTL::CommandQueue *p_queue) {
		struct TriangleVertex {
			float x, y, z;
		};
		const TriangleVertex vertices[] = {
			{ -1.0f, -1.0f, 0.0f },
			{ 1.0f, -1.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
		};
		vertex_buffer = NS::TransferPtr(p_device->newBuffer(vertices, sizeof(vertices), MTL::ResourceStorageModeShared));
		if (!vertex_buffer) {
			return false;
		}

		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> geometry = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		geometry->setVertexBuffer(vertex_buffer.get());
		geometry->setVertexBufferOffset(0);
		geometry->setVertexStride(sizeof(TriangleVertex));
		geometry->setTriangleCount(1);
		geometry->setOpaque(true);

		NS::Object *geometry_object = geometry.get();
		NS::SharedPtr<NS::Array> geometries = NS::TransferPtr(NS::Array::array(&geometry_object, 1)->retain());
		NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> blas_descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
		blas_descriptor->setGeometryDescriptors(geometries.get());

		MTL::AccelerationStructureSizes blas_sizes = p_device->accelerationStructureSizes(blas_descriptor.get());
		MDAccelerationStructure blas_structure(MDAccelerationStructure::Type::BLAS, blas_descriptor, blas_sizes, {});
		if (!blas_structure.allocate(p_device)) {
			return false;
		}
		NS::SharedPtr<MTL::Buffer> blas_scratch = NS::TransferPtr(p_device->newBuffer(blas_structure.build_scratch_size, MTL::ResourceStorageModePrivate));
		if (!blas_scratch) {
			return false;
		}
		NS::SharedPtr<MTL::CommandBuffer> blas_command = NS::RetainPtr(p_queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> blas_encoder = NS::RetainPtr(blas_command->accelerationStructureCommandEncoder());
		blas_structure.encode_build(blas_encoder.get(), blas_scratch.get());
		blas_encoder->endEncoding();
		blas_command->commit();
		blas_command->waitUntilCompleted();
		if (blas_command->status() != MTL::CommandBufferStatusCompleted) {
			return false;
		}
		blas = blas_structure.accel;

		RDD::AccelerationStructureInstance instance;
		instance.id = 42;
		instance.mask = 0xff;
		instance.hit_sbt_offset = 0;
		instance.flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT);
		instance.flags.set_flag(RDD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT);
		instance.blas = RDD::AccelerationStructureID(&blas_structure);

		MDAccelerationStructureInstance metal_instance;
		if (!metal_instance.write(instance)) {
			return false;
		}
		instance_buffer = NS::TransferPtr(p_device->newBuffer(&metal_instance, sizeof(metal_instance), MTL::ResourceStorageModeShared));
		if (!instance_buffer) {
			return false;
		}

		NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
		tlas_descriptor->setInstanceCount(1);
		tlas_descriptor->setInstanceDescriptorStride(sizeof(MDAccelerationStructureInstance));
		MTL::AccelerationStructureSizes tlas_sizes = p_device->accelerationStructureSizes(tlas_descriptor.get());
		MDAccelerationStructure tlas_structure(MDAccelerationStructure::Type::TLAS, tlas_descriptor, tlas_sizes, {}, 1);
		if (!tlas_structure.allocate(p_device)) {
			return false;
		}
		if (!tlas_structure.prepare_tlas_build(instance_buffer.get(), 0, 1)) {
			return false;
		}
		NS::SharedPtr<MTL::Buffer> tlas_scratch = NS::TransferPtr(p_device->newBuffer(tlas_structure.build_scratch_size, MTL::ResourceStorageModePrivate));
		if (!tlas_scratch) {
			return false;
		}
		NS::SharedPtr<MTL::CommandBuffer> tlas_command = NS::RetainPtr(p_queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> tlas_encoder = NS::RetainPtr(tlas_command->accelerationStructureCommandEncoder());
		tlas_structure.encode_build(tlas_encoder.get(), tlas_scratch.get());
		tlas_encoder->endEncoding();
		tlas_command->commit();
		tlas_command->waitUntilCompleted();
		if (tlas_command->status() != MTL::CommandBufferStatusCompleted) {
			return false;
		}
		tlas = tlas_structure.accel;
		return true;
	}
};

// Compiles p_msl_source, dispatches two rays against the scene, and checks the
// hit/miss contract from the testing standard's GPU functional section.
static void run_trace_kernel(MTL::Device *p_device, MTL::CommandQueue *p_queue, SpikeScene &p_scene,
		const String &p_msl_source, const char *p_entry_point, MTL::LanguageVersion p_language_version, const char *p_label) {
	NS::Error *error = nullptr;
	NS::SharedPtr<MTL::CompileOptions> compile_options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
	compile_options->setLanguageVersion(p_language_version);
	CharString msl_utf8 = p_msl_source.utf8();
	NS::SharedPtr<NS::String> source_string = NS::TransferPtr(NS::String::alloc()->init(msl_utf8.get_data(), NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::Library> library = NS::TransferPtr(p_device->newLibrary(source_string.get(), compile_options.get(), &error));
	if (!library) {
		FAIL(vformat("%s: MSL runtime compile failed: %s", p_label, error ? error->localizedDescription()->utf8String() : "unknown error"));
		return;
	}

	NS::SharedPtr<NS::String> entry_name = NS::TransferPtr(NS::String::alloc()->init(p_entry_point, NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::Function> function = NS::TransferPtr(library->newFunction(entry_name.get()));
	REQUIRE(function);
	NS::SharedPtr<MTL::ComputePipelineState> pipeline = NS::TransferPtr(p_device->newComputePipelineState(function.get(), &error));
	if (!pipeline) {
		FAIL(vformat("%s: compute pipeline creation failed: %s", p_label, error ? error->localizedDescription()->utf8String() : "unknown error"));
		return;
	}

	SpikeRayData rays;
	SpikeResultData results;
	NS::SharedPtr<MTL::Buffer> ray_buffer = NS::TransferPtr(p_device->newBuffer(&rays, sizeof(rays), MTL::ResourceStorageModeShared));
	NS::SharedPtr<MTL::Buffer> result_buffer = NS::TransferPtr(p_device->newBuffer(&results, sizeof(results), MTL::ResourceStorageModeShared));
	REQUIRE(ray_buffer);
	REQUIRE(result_buffer);

	NS::SharedPtr<MTL::CommandBuffer> command = NS::RetainPtr(p_queue->commandBuffer());
	NS::SharedPtr<MTL::ComputeCommandEncoder> encoder = NS::RetainPtr(command->computeCommandEncoder());
	REQUIRE(encoder);
	encoder->setComputePipelineState(pipeline.get());
	encoder->setAccelerationStructure(p_scene.tlas.get(), 0);
	encoder->setBuffer(ray_buffer.get(), 0, 1);
	encoder->setBuffer(result_buffer.get(), 0, 2);
	// The TLAS references the BLAS indirectly; it must be made resident.
	encoder->useResource(p_scene.blas.get(), MTL::ResourceUsageRead);
	encoder->dispatchThreadgroups(MTL::Size(1, 1, 1), MTL::Size(2, 1, 1));
	encoder->endEncoding();
	command->commit();
	command->waitUntilCompleted();
	REQUIRE(command->status() == MTL::CommandBufferStatusCompleted);
	CHECK(command->error() == nullptr);

	memcpy(&results, result_buffer->contents(), sizeof(results));
	const float *hit = results.hit_t_primitive_instance[0];
	const float *miss = results.hit_t_primitive_instance[1];
	CHECK(hit[0] == 1.0f);
	CHECK(hit[1] == doctest::Approx(2.0f).epsilon(0.001));
	CHECK(hit[2] == 0.0f);
	CHECK(hit[3] == 0.0f);
	CHECK(miss[0] == 0.0f);
	CHECK(miss[1] == -1.0f);
	print_line(vformat("MetalRT SHADER_LOWERING shader spike: device=\"%s\" lane=%s hit=(%f, t=%f, prim=%f, inst=%f) miss=%f",
			p_device->name()->utf8String(), p_label, hit[0], hit[1], hit[2], hit[3], miss[0]));
}

static bool run_intersector_patch_lane(MTL::Device *p_device, MTL::CommandQueue *p_queue, SpikeScene &p_scene,
		MTL::Library *p_library, uint32_t p_rt_flags, INTERSECTORResultData &r_results, String &r_error) {
	NS::SharedPtr<NS::String> entry_name = NS::TransferPtr(NS::String::alloc()->init("intersector_trace_parity", NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::FunctionConstantValues> constants = NS::TransferPtr(MTL::FunctionConstantValues::alloc()->init());
	constants->setConstantValue(&p_rt_flags, MTL::DataTypeUInt, NS::UInteger(0));
	NS::Error *error = nullptr;
	NS::SharedPtr<MTL::Function> function = NS::TransferPtr(p_library->newFunction(entry_name.get(), constants.get(), &error));
	if (!function) {
		r_error = vformat("function specialization failed: %s", error ? error->localizedDescription()->utf8String() : "unknown error");
		return false;
	}
	NS::SharedPtr<MTL::ComputePipelineState> pipeline = NS::TransferPtr(p_device->newComputePipelineState(function.get(), &error));
	if (!pipeline) {
		r_error = vformat("pipeline creation failed: %s", error ? error->localizedDescription()->utf8String() : "unknown error");
		return false;
	}

	INTERSECTORRayData rays;
	NS::SharedPtr<MTL::Buffer> ray_buffer = NS::TransferPtr(p_device->newBuffer(&rays, sizeof(rays), MTL::ResourceStorageModeShared));
	NS::SharedPtr<MTL::Buffer> result_buffer = NS::TransferPtr(p_device->newBuffer(&r_results, sizeof(r_results), MTL::ResourceStorageModeShared));
	if (!ray_buffer || !result_buffer) {
		r_error = "buffer allocation failed";
		return false;
	}

	NS::SharedPtr<MTL::CommandBuffer> command = NS::RetainPtr(p_queue->commandBuffer());
	NS::SharedPtr<MTL::ComputeCommandEncoder> encoder = NS::RetainPtr(command->computeCommandEncoder());
	if (!encoder) {
		r_error = "compute encoder creation failed";
		return false;
	}
	encoder->setComputePipelineState(pipeline.get());
	encoder->setAccelerationStructure(p_scene.tlas.get(), 0);
	encoder->setBuffer(ray_buffer.get(), 0, 1);
	encoder->setBuffer(result_buffer.get(), 0, 2);
	encoder->useResource(p_scene.blas.get(), MTL::ResourceUsageRead);
	encoder->dispatchThreadgroups(MTL::Size(1, 1, 1), MTL::Size(4, 1, 1));
	encoder->endEncoding();
	command->commit();
	command->waitUntilCompleted();
	if (command->status() != MTL::CommandBufferStatusCompleted || command->error() != nullptr) {
		r_error = vformat("dispatch failed: %s", command->error() ? command->error()->localizedDescription()->utf8String() : "unknown error");
		return false;
	}
	memcpy(&r_results, result_buffer->contents(), sizeof(r_results));
	return true;
}

TEST_CASE("[MetalRT] SHADER_LOWERING SPIRV-Cross lane lowers ray query compute to MSL") {
	String glsl_error;
	Vector<uint8_t> spirv = compile_stage_to_spirv(RDC::SHADER_STAGE_COMPUTE, RAY_QUERY_COMPUTE_GLSL, &glsl_error);
	REQUIRE_MESSAGE(!spirv.is_empty(), vformat("glslang failed: %s", glsl_error));

	// Classic (non-argument-buffer) bindings at the SPIRV-Cross ray-query
	// floor, MSL 2.4.
	MetalRTShaderLowering::Result classic = MetalRTShaderLowering::lower_spirv(RDC::SHADER_STAGE_COMPUTE, spirv, 2, 4, false, false);
	REQUIRE_MESSAGE(classic.ok, vformat("SPIRV-Cross lowering failed: %s", classic.error));
	CHECK(classic.msl_source.contains("intersection_query"));
	CHECK(classic.msl_source.contains("acceleration_structure"));
	CHECK(classic.entry_point == "main0");

	// Tier-2 argument buffers exactly as the container configures them
	// (pad_argument_buffer_resources on). PIPELINE_MAPPING teaches the small vendored padding
	// switch that acceleration structures occupy the buffer-index namespace.
	MetalRTShaderLowering::Result padded = MetalRTShaderLowering::lower_spirv(RDC::SHADER_STAGE_COMPUTE, spirv, 3, 0, true, true);
	REQUIRE_MESSAGE(padded.ok, vformat("SPIRV-Cross padded argument-buffer lowering failed: %s", padded.error));
	CHECK(padded.msl_source.contains("acceleration_structure"));

	// Tier-2 argument buffers with padding disabled lower cleanly.
	MetalRTShaderLowering::Result argbuf = MetalRTShaderLowering::lower_spirv(RDC::SHADER_STAGE_COMPUTE, spirv, 3, 0, true, false);
	REQUIRE_MESSAGE(argbuf.ok, vformat("SPIRV-Cross argument-buffer lowering failed: %s", argbuf.error));
	CHECK(argbuf.msl_source.contains("intersection_query"));
}

TEST_CASE("[MetalRT] TRAVERSAL_METADATA rejects pre-v3 Metal shader containers before stage decoding") {
	String glsl_error;
	Vector<uint8_t> spirv = compile_stage_to_spirv(RDC::SHADER_STAGE_COMPUTE, RAY_QUERY_COMPUTE_GLSL, &glsl_error);
	REQUIRE_MESSAGE(!spirv.is_empty(), vformat("glslang failed: %s", glsl_error));

	const MetalDeviceProfile *profile = MetalDeviceProfile::get_profile(
			MetalDeviceProfile::Platform::macOS, MetalDeviceProfile::GPU::Apple8, os_version::MACOS_13_0);
	REQUIRE(profile != nullptr);
	Ref<RenderingShaderContainerMetal> current;
	current.instantiate();
	current->set_device_profile(profile);
	Vector<RDC::ShaderStageSPIRVData> stages;
	stages.resize(1);
	stages.write[0].shader_stage = RDC::SHADER_STAGE_COMPUTE;
	stages.write[0].spirv = spirv;
	REQUIRE(current->set_code_from_spirv("TRAVERSAL_METADATA_cache_version_probe", stages));

	PackedByteArray bytes = current->to_bytes();
	REQUIRE(bytes.size() >= 16);
	const uint32_t old_format_version = 2;
	memcpy(bytes.ptrw() + 12, &old_format_version, sizeof(old_format_version));

	Ref<RenderingShaderContainerMetal> old;
	old.instantiate();
	old->set_device_profile(profile);
	ERR_PRINT_OFF;
	const bool loaded = old->from_bytes(bytes);
	ERR_PRINT_ON;
	CHECK_FALSE(loaded);
}

TEST_CASE("[MetalRT] INTERSECTOR production MSL rewrite injects closest-hit and shadow intersectors") {
	std::string source = load_msl_rewrite_fixture("intersector_scene_fixture.metal");
	REQUIRE_FALSE(source.empty());

	MetalRTShaderLowering::IntersectorPatchResult patch = MetalRTShaderLowering::patch_scene_ray_query_to_intersector(source);
	REQUIRE_MESSAGE(patch.applied(), patch.detail);
	CHECK(String(MetalRTShaderLowering::intersector_patch_status_name(patch.status)) == "applied");

	// The function-constant gate keeps alpha/custom material variants on their
	// original query path. Only ALL_OPAQUE specializations force opacity.
	CHECK(source.find("constant bool godot_use_intersector = ((RT_FLAGS & 16u) != 0u)") != std::string::npos);
	CHECK(source.find("trace.force_opacity(raytracing::forced_opacity::opaque)") != std::string::npos);
	CHECK(source.find("return godot_trace_material_intersector(origin, direction, max_distance, hit, tlas)") != std::string::npos);
	CHECK(source.find("return godot_trace_shadow_blocked_intersector(origin, direction, max_distance, tlas)") != std::string::npos);

	// Match the GLSL/Blender contracts: closest hit does not terminate early,
	// shadows do, and both preserve the explicit range, mask, and back-face
	// policy. Per-instance DisableTriangleCulling still makes double-sided
	// geometry visible; Metal applies TLAS transforms before returning IDs.
	CHECK(source.find("raytracing::ray r(origin, direction, 0.001, max_distance)") != std::string::npos);
	CHECK(source.find("trace.set_triangle_cull_mode(raytracing::triangle_cull_mode::back)") != std::string::npos);
	CHECK(source.find("trace.accept_any_intersection(false)") != std::string::npos);
	CHECK(source.find("trace.accept_any_intersection(true)") != std::string::npos);
	CHECK(source.find("trace.intersect(r, tlas, 0xFFu)") != std::string::npos);
	CHECK(source.find("hit.geometry_idx = result.user_instance_id") != std::string::npos);
	CHECK(source.find("hit.primitive_idx = result.primitive_id") != std::string::npos);
	CHECK(source.find("hit.barycentrics = result.triangle_barycentric_coord") != std::string::npos);
	CHECK(source.find("hit.front_face = short(result.triangle_front_facing)") != std::string::npos);

	// Injection must precede SPIRV-Cross attributes, never split an attribute
	// from its function definition.
	size_t helper = source.find("godot_trace_material_intersector");
	size_t material_attribute = source.find("static inline __attribute__((always_inline))");
	size_t material_definition = source.find("bool trace_material(");
	REQUIRE(helper != std::string::npos);
	REQUIRE(material_attribute != std::string::npos);
	REQUIRE(material_definition != std::string::npos);
	CHECK(helper < material_attribute);
	CHECK(material_attribute < material_definition);
}

TEST_CASE("[MetalRT] INTERSECTOR production MSL rewrite reports transactional fallbacks") {
	SUBCASE("SPIRV-Cross brace drift") {
		std::string source = load_msl_rewrite_fixture("intersector_anchor_drift_fixture.metal");
		REQUIRE_FALSE(source.empty());
		const std::string original = source;
		MetalRTShaderLowering::IntersectorPatchResult patch = MetalRTShaderLowering::patch_scene_ray_query_to_intersector(source);
		CHECK_FALSE(patch.applied());
		CHECK(patch.status == MetalRTShaderLowering::IntersectorPatchStatus::TRACE_MATERIAL_LAYOUT);
		CHECK(String(MetalRTShaderLowering::intersector_patch_status_name(patch.status)) == "trace_material_layout");
		CHECK(source == original);
	}

	SUBCASE("procedural/AABB variant") {
		std::string source = load_msl_rewrite_fixture("intersector_procedural_fixture.metal");
		REQUIRE_FALSE(source.empty());
		const std::string original = source;
		MetalRTShaderLowering::IntersectorPatchResult patch = MetalRTShaderLowering::patch_scene_ray_query_to_intersector(source);
		CHECK_FALSE(patch.applied());
		CHECK(patch.status == MetalRTShaderLowering::IntersectorPatchStatus::PROCEDURAL_GEOMETRY);
		CHECK(String(MetalRTShaderLowering::intersector_patch_status_name(patch.status)) == "procedural_geometry");
		CHECK(source == original);
	}

	SUBCASE("missing function constant") {
		std::string source = load_msl_rewrite_fixture("intersector_scene_fixture.metal");
		REQUIRE_FALSE(source.empty());
		size_t begin = source.find("constant uint RT_FLAGS ");
		REQUIRE(begin != std::string::npos);
		size_t end = source.find('\n', begin);
		source.erase(begin, end - begin + 1);
		const std::string original = source;
		MetalRTShaderLowering::IntersectorPatchResult patch = MetalRTShaderLowering::patch_scene_ray_query_to_intersector(source);
		CHECK_FALSE(patch.applied());
		CHECK(patch.status == MetalRTShaderLowering::IntersectorPatchStatus::MISSING_RT_FLAGS);
		CHECK(source == original);
	}

	SUBCASE("shadow layout drift after valid material anchor") {
		std::string source = load_msl_rewrite_fixture("intersector_scene_fixture.metal");
		REQUIRE_FALSE(source.empty());
		size_t shadow = source.find("bool trace_shadow_blocked(");
		REQUIRE(shadow != std::string::npos);
		size_t brace = source.find(")\n{\n", shadow);
		REQUIRE(brace != std::string::npos);
		source.replace(brace, 3, ") {\n");
		const std::string original = source;
		MetalRTShaderLowering::IntersectorPatchResult patch = MetalRTShaderLowering::patch_scene_ray_query_to_intersector(source);
		CHECK_FALSE(patch.applied());
		CHECK(patch.status == MetalRTShaderLowering::IntersectorPatchStatus::TRACE_SHADOW_LAYOUT);
		CHECK(source == original);
	}
}

TEST_CASE("[MetalRT] Metal container lowers bindless GPU-addressed material access") {
	String glsl_error;
	Vector<uint8_t> spirv = compile_stage_to_spirv(RDC::SHADER_STAGE_COMPUTE, BINDLESS_MATERIAL_GLSL, &glsl_error);
	REQUIRE_MESSAGE(!spirv.is_empty(), vformat("glslang failed: %s", glsl_error));

	Ref<RenderingShaderContainerMetal> container;
	String msl_source;
	REQUIRE(build_bindless_material_container(spirv, container, msl_source));

	RDC::ShaderReflection reflection = container->get_shader_reflection();
	REQUIRE(reflection.uniform_sets.size() == 2);
	REQUIRE(reflection.uniform_sets[1].size() == 1);
	CHECK(reflection.uniform_sets[1][0].type == RDC::UNIFORM_TYPE_TEXTURE);
	CHECK(reflection.uniform_sets[1][0].unbounded);

	RenderingShaderContainerMetal::MetalShaderReflection metal_reflection = container->get_metal_shader_reflection();
	REQUIRE(metal_reflection.uniform_sets.size() == 2);
	REQUIRE(metal_reflection.uniform_sets[1].size() == 1);
	CHECK(metal_reflection.uniform_sets[1][0].array_length == RenderingShaderContainerMetal::UniformData::UNBOUNDED_ARRAY_LENGTH);
	CHECK(metal_reflection.uniform_sets[1][0].arg_buffer.texture == 0);
	CHECK(container->mtl_reflection_data.uses_argument_buffers());

	CHECK(msl_source.contains("spvDescriptorArray"));
	CHECK(msl_source.contains("device spvDescriptorSetBuffer1"));
	CHECK(msl_source.contains("device Material"));
}

TEST_CASE_PENDING("[MetalRT][GPU] Metal container executes bindless GPU-addressed material access") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsFamily(MTL::GPUFamilyMetal3) || device->argumentBuffersSupport() != MTL::ArgumentBuffersTier2) {
		MESSAGE("SKIP_REASON=missing_metal3_argument_buffers");
		return;
	}
	if (!__builtin_available(macOS 13.0, *)) {
		MESSAGE("SKIP_REASON=missing_gpu_address_os_support");
		return;
	}

	String glsl_error;
	Vector<uint8_t> spirv = compile_stage_to_spirv(RDC::SHADER_STAGE_COMPUTE, BINDLESS_MATERIAL_GLSL, &glsl_error);
	REQUIRE_MESSAGE(!spirv.is_empty(), vformat("glslang failed: %s", glsl_error));

	Ref<RenderingShaderContainerMetal> container;
	String msl_source;
	REQUIRE(build_bindless_material_container(spirv, container, msl_source));

	NS::Error *error = nullptr;
	NS::SharedPtr<MTL::CompileOptions> compile_options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
	compile_options->setLanguageVersion(MTL::LanguageVersion3_0);
	CharString source_utf8 = msl_source.utf8();
	NS::SharedPtr<NS::String> source_string = NS::TransferPtr(NS::String::alloc()->init(source_utf8.get_data(), NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::Library> library = NS::TransferPtr(device->newLibrary(source_string.get(), compile_options.get(), &error));
	REQUIRE_MESSAGE(library, vformat("MSL runtime compile failed: %s", error ? error->localizedDescription()->utf8String() : "unknown error"));
	NS::SharedPtr<NS::String> entry_name = NS::TransferPtr(NS::String::alloc()->init("main0", NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::Function> function = NS::TransferPtr(library->newFunction(entry_name.get()));
	REQUIRE(function);
	NS::SharedPtr<MTL::ComputePipelineState> pipeline = NS::TransferPtr(device->newComputePipelineState(function.get(), &error));
	REQUIRE_MESSAGE(pipeline, vformat("compute pipeline creation failed: %s", error ? error->localizedDescription()->utf8String() : "unknown error"));

	struct MaterialData {
		uint32_t texture_index = 1;
	};
	MaterialData material;
	NS::SharedPtr<MTL::Buffer> material_buffer = NS::TransferPtr(device->newBuffer(&material, sizeof(material), MTL::ResourceStorageModeShared));
	REQUIRE(material_buffer);
	uint64_t material_address = material_buffer->gpuAddress();
	NS::SharedPtr<MTL::Buffer> address_buffer = NS::TransferPtr(device->newBuffer(&material_address, sizeof(material_address), MTL::ResourceStorageModeShared));
	REQUIRE(address_buffer);
	float result_data[4] = {};
	NS::SharedPtr<MTL::Buffer> result_buffer = NS::TransferPtr(device->newBuffer(result_data, sizeof(result_data), MTL::ResourceStorageModeShared));
	REQUIRE(result_buffer);

	NS::SharedPtr<MTL::TextureDescriptor> texture_descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
	texture_descriptor->setTextureType(MTL::TextureType2D);
	texture_descriptor->setPixelFormat(MTL::PixelFormatRGBA32Float);
	texture_descriptor->setWidth(1);
	texture_descriptor->setHeight(1);
	texture_descriptor->setStorageMode(MTL::StorageModeShared);
	texture_descriptor->setUsage(MTL::TextureUsageShaderRead);
	NS::SharedPtr<MTL::Texture> sentinel_texture = NS::TransferPtr(device->newTexture(texture_descriptor.get()));
	NS::SharedPtr<MTL::Texture> material_texture = NS::TransferPtr(device->newTexture(texture_descriptor.get()));
	REQUIRE(sentinel_texture);
	REQUIRE(material_texture);
	const float sentinel_color[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
	const float expected_color[4] = { 0.125f, 0.75f, 0.25f, 1.0f };
	sentinel_texture->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, sentinel_color, sizeof(sentinel_color));
	material_texture->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, expected_color, sizeof(expected_color));

	NS::SharedPtr<MTL::SamplerDescriptor> sampler_descriptor = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
	sampler_descriptor->setSupportArgumentBuffers(true);
	sampler_descriptor->setMinFilter(MTL::SamplerMinMagFilterNearest);
	sampler_descriptor->setMagFilter(MTL::SamplerMinMagFilterNearest);
	NS::SharedPtr<MTL::SamplerState> sampler = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
	REQUIRE(sampler);

	RenderingShaderContainerMetal::MetalShaderReflection reflection = container->get_metal_shader_reflection();
	REQUIRE(reflection.uniform_sets.size() == 2);
	REQUIRE(reflection.uniform_sets[0].size() == 3);
	REQUIRE(reflection.uniform_sets[1].size() == 1);
	const RenderingShaderContainerMetal::UniformData &addresses_uniform = reflection.uniform_sets[0][0];
	const RenderingShaderContainerMetal::UniformData &sampler_uniform = reflection.uniform_sets[0][1];
	const RenderingShaderContainerMetal::UniformData &result_uniform = reflection.uniform_sets[0][2];
	const RenderingShaderContainerMetal::UniformData &textures_uniform = reflection.uniform_sets[1][0];

	uint64_t fixed_argument_data[3] = {};
	fixed_argument_data[addresses_uniform.arg_buffer.buffer] = address_buffer->gpuAddress();
	fixed_argument_data[sampler_uniform.arg_buffer.sampler] = sampler->gpuResourceID()._impl;
	fixed_argument_data[result_uniform.arg_buffer.buffer] = result_buffer->gpuAddress();
	NS::SharedPtr<MTL::Buffer> fixed_argument_buffer = NS::TransferPtr(device->newBuffer(fixed_argument_data, sizeof(fixed_argument_data), MTL::ResourceStorageModeShared));
	REQUIRE(fixed_argument_buffer);

	Vector<uint64_t> bindless_argument_data;
	bindless_argument_data.resize(textures_uniform.arg_buffer.texture + 2);
	bindless_argument_data.write[textures_uniform.arg_buffer.texture + 0] = sentinel_texture->gpuResourceID()._impl;
	bindless_argument_data.write[textures_uniform.arg_buffer.texture + 1] = material_texture->gpuResourceID()._impl;
	NS::SharedPtr<MTL::Buffer> bindless_argument_buffer = NS::TransferPtr(device->newBuffer(bindless_argument_data.ptr(), bindless_argument_data.size() * sizeof(uint64_t), MTL::ResourceStorageModeShared));
	REQUIRE(bindless_argument_buffer);

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);
	NS::SharedPtr<MTL::CommandBuffer> command = NS::RetainPtr(queue->commandBuffer());
	NS::SharedPtr<MTL::ComputeCommandEncoder> encoder = NS::RetainPtr(command->computeCommandEncoder());
	REQUIRE(encoder);
	encoder->setComputePipelineState(pipeline.get());
	encoder->setBuffer(fixed_argument_buffer.get(), 0, 0);
	encoder->setBuffer(bindless_argument_buffer.get(), 0, 1);
	encoder->useResource(address_buffer.get(), MTL::ResourceUsageRead);
	encoder->useResource(material_buffer.get(), MTL::ResourceUsageRead);
	encoder->useResource(result_buffer.get(), MTL::ResourceUsageWrite);
	encoder->useResource(sentinel_texture.get(), MTL::ResourceUsageRead);
	encoder->useResource(material_texture.get(), MTL::ResourceUsageRead);
	encoder->dispatchThreadgroups(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
	encoder->endEncoding();
	command->commit();
	command->waitUntilCompleted();
	REQUIRE(command->status() == MTL::CommandBufferStatusCompleted);
	CHECK(command->error() == nullptr);

	memcpy(result_data, result_buffer->contents(), sizeof(result_data));
	for (uint32_t i = 0; i < 4; i++) {
		CHECK(result_data[i] == doctest::Approx(expected_color[i]));
	}
	print_line(vformat("MetalRT bindless material access: device=\"%s\" descriptors=%d color=(%f, %f, %f, %f)",
			device->name()->utf8String(), bindless_argument_data.size(), result_data[0], result_data[1], result_data[2], result_data[3]));
}

TEST_CASE("[MetalRT] SHADER_LOWERING SPIRV-Cross lane cannot lower RT pipeline stages") {
	struct StageExperiment {
		RDC::ShaderStage stage;
		const char *name;
		const char *glsl;
	};
	const StageExperiment experiments[] = {
		{ RDC::SHADER_STAGE_RAYGEN, "raygen", RAYGEN_GLSL },
		{ RDC::SHADER_STAGE_CLOSEST_HIT, "closest_hit", CLOSEST_HIT_GLSL },
	};

	for (const StageExperiment &experiment : experiments) {
		String glsl_error;
		Vector<uint8_t> spirv = compile_stage_to_spirv(experiment.stage, experiment.glsl, &glsl_error);
		REQUIRE_MESSAGE(!spirv.is_empty(), vformat("glslang failed for %s: %s", experiment.name, glsl_error));

		// Padding is disabled so the failure reflects the execution model, not
		// the acceleration-structure padding gap measured above.
		MetalRTShaderLowering::Result lowered = MetalRTShaderLowering::lower_spirv(experiment.stage, spirv, 3, 0, true, false);
		// A7 evidence: the MSL backend has no RT-pipeline execution models. If
		// this ever starts succeeding with valid MSL, the strategy in
		// docs/rt_metal_port/shader_strategy.md must be revisited.
		bool produced_valid_msl = lowered.ok && !lowered.msl_source.contains("unknown ");
		CHECK_MESSAGE(!produced_valid_msl, vformat("SPIRV-Cross unexpectedly lowered %s stage; revisit shader strategy", experiment.name));
		MESSAGE(vformat("SHADER_LOWERING %s lowering outcome: ok=%s error=\"%s\"", experiment.name, lowered.ok ? "true" : "false", lowered.error));
	}
}

TEST_CASE_PENDING("[MetalRT][GPU] SHADER_LOWERING SPIRV-Cross ray query kernel traces hit and miss") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	String glsl_error;
	Vector<uint8_t> spirv = compile_stage_to_spirv(RDC::SHADER_STAGE_COMPUTE, RAY_QUERY_COMPUTE_GLSL, &glsl_error);
	REQUIRE_MESSAGE(!spirv.is_empty(), vformat("glslang failed: %s", glsl_error));
	MetalRTShaderLowering::Result lowered = MetalRTShaderLowering::lower_spirv(RDC::SHADER_STAGE_COMPUTE, spirv, 2, 4, false, false);
	REQUIRE_MESSAGE(lowered.ok, vformat("SPIRV-Cross lowering failed: %s", lowered.error));

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);
	SpikeScene scene;
	REQUIRE(scene.build(device.get(), queue.get()));

	CharString entry_utf8 = lowered.entry_point.utf8();
	run_trace_kernel(device.get(), queue.get(), scene, lowered.msl_source, entry_utf8.get_data(), MTL::LanguageVersion2_4, "spirv_cross_ray_query");
}

TEST_CASE_PENDING("[MetalRT][GPU] SHADER_LOWERING native intersector kernel traces hit and miss") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);
	SpikeScene scene;
	REQUIRE(scene.build(device.get(), queue.get()));

	run_trace_kernel(device.get(), queue.get(), scene, String::utf8(NATIVE_INTERSECTOR_MSL), "trace_spike", MTL::LanguageVersion2_3, "native_intersector");
}

TEST_CASE_PENDING("[MetalRT][GPU] INTERSECTOR production rewrite matches query traversal") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	std::string patched_source(INTERSECTOR_PATCH_PARITY_MSL);
	MetalRTShaderLowering::IntersectorPatchResult patch = MetalRTShaderLowering::patch_scene_ray_query_to_intersector(patched_source);
	REQUIRE_MESSAGE(patch.applied(), patch.detail);
	CHECK(patched_source.find("intersection_query") != std::string::npos);
	CHECK(patched_source.find("godot_trace_material_intersector") != std::string::npos);

	NS::Error *error = nullptr;
	NS::SharedPtr<MTL::CompileOptions> compile_options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
	compile_options->setLanguageVersion(MTL::LanguageVersion2_4);
	NS::SharedPtr<NS::String> source_string = NS::TransferPtr(NS::String::alloc()->init(patched_source.c_str(), NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::Library> library = NS::TransferPtr(device->newLibrary(source_string.get(), compile_options.get(), &error));
	REQUIRE_MESSAGE(library, vformat("INTERSECTOR patched MSL compile failed: %s", error ? error->localizedDescription()->utf8String() : "unknown error"));

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);
	SpikeScene scene;
	REQUIRE(scene.build(device.get(), queue.get()));

	INTERSECTORResultData query_results;
	INTERSECTORResultData intersector_results;
	String lane_error;
	REQUIRE_MESSAGE(run_intersector_patch_lane(device.get(), queue.get(), scene, library.get(), 0u, query_results, lane_error), lane_error);
	REQUIRE_MESSAGE(run_intersector_patch_lane(device.get(), queue.get(), scene, library.get(), 16u, intersector_results, lane_error), lane_error);

	for (uint32_t ray_index = 0; ray_index < 4; ray_index++) {
		for (uint32_t component = 0; component < 4; component++) {
			CHECK(intersector_results.material_shadow[ray_index][component] == doctest::Approx(query_results.material_shadow[ray_index][component]));
		}
	}
	CHECK(query_results.material_shadow[0][0] == 1.0f);
	CHECK(query_results.material_shadow[0][1] == doctest::Approx(2.0f).epsilon(0.001));
	CHECK(query_results.material_shadow[0][2] == 1.0f);
	CHECK(query_results.material_shadow[0][3] == 0.0f);
	for (uint32_t ray_index = 1; ray_index < 4; ray_index++) {
		CHECK(query_results.material_shadow[ray_index][0] == 0.0f);
		CHECK(query_results.material_shadow[ray_index][1] == -1.0f);
		CHECK(query_results.material_shadow[ray_index][2] == 0.0f);
		CHECK(query_results.material_shadow[ray_index][3] == -1.0f);
	}
	print_line(vformat("MetalRT INTERSECTOR traversal parity: device=\"%s\" rays=4 closest=passed shadow=passed t_min=passed t_max=passed",
			device->name()->utf8String()));
}

} // namespace TestMetalRTShaderStrategy

#endif // METAL_ENABLED && MODULE_GLSLANG_ENABLED
