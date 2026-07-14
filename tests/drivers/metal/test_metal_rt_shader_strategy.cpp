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
/* "Software"), to deal in the Software without restriction, including  */
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

#include "drivers/metal/metal_objects_shared.h"
#include "drivers/metal/metal_rt_shader_lowering.h"
#include "modules/glslang/shader_compile.h"

namespace TestMetalRTShaderStrategy {

// Chunk C7 shader-lowering spike. These tests provide the empirical evidence
// behind docs/rt_metal_port/shader_strategy.md: which RT shader forms the
// existing SPIRV-Cross lane can lower to MSL, and that the native
// metal::raytracing::intersector lane executes correctly against the C5/C6
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

static Vector<uint8_t> compile_stage_to_spirv(RDC::ShaderStage p_stage, const char *p_glsl, String *r_error) {
	return compile_glslang_shader(p_stage, String::utf8(p_glsl),
			RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_3, RDC::SHADER_SPIRV_VERSION_1_6, r_error);
}

// Builds the C5/C6 single-triangle BLAS + one-instance TLAS scene and leaves
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
	print_line(vformat("MetalRT C7 shader spike: device=\"%s\" lane=%s hit=(%f, t=%f, prim=%f, inst=%f) miss=%f",
			p_device->name()->utf8String(), p_label, hit[0], hit[1], hit[2], hit[3], miss[0]));
}

TEST_CASE("[MetalRT] C7 SPIRV-Cross lane lowers ray query compute to MSL") {
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
	// (pad_argument_buffer_resources on): SPIRV-Cross rejects the
	// acceleration-structure binding outright. If this starts passing, the
	// vendored SPIRV-Cross gained AS padding support and
	// docs/rt_metal_port/shader_strategy.md should be updated.
	MetalRTShaderLowering::Result padded = MetalRTShaderLowering::lower_spirv(RDC::SHADER_STAGE_COMPUTE, spirv, 3, 0, true, true);
	CHECK_FALSE(padded.ok);
	CHECK(padded.error.contains("Unexpected argument buffer resource base type"));

	// Tier-2 argument buffers with padding disabled lower cleanly.
	MetalRTShaderLowering::Result argbuf = MetalRTShaderLowering::lower_spirv(RDC::SHADER_STAGE_COMPUTE, spirv, 3, 0, true, false);
	REQUIRE_MESSAGE(argbuf.ok, vformat("SPIRV-Cross argument-buffer lowering failed: %s", argbuf.error));
	CHECK(argbuf.msl_source.contains("intersection_query"));
}

TEST_CASE("[MetalRT] C7 SPIRV-Cross lane cannot lower RT pipeline stages") {
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
		MESSAGE(vformat("C7 %s lowering outcome: ok=%s error=\"%s\"", experiment.name, lowered.ok ? "true" : "false", lowered.error));
	}
}

TEST_CASE_PENDING("[MetalRT][GPU] C7 SPIRV-Cross ray query kernel traces hit and miss") {
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

TEST_CASE_PENDING("[MetalRT][GPU] C7 native intersector kernel traces hit and miss") {
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

} // namespace TestMetalRTShaderStrategy

#endif // METAL_ENABLED && MODULE_GLSLANG_ENABLED
