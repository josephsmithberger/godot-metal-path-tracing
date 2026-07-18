/**************************************************************************/
/*  test_metal_rt_pipeline.cpp                                            */
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

TEST_FORCE_LINK(test_metal_rt_pipeline)

#ifdef METAL_ENABLED

#include "drivers/metal/metal_objects_shared.h"

namespace TestMetalRTPipeline {

struct SyntheticGroups {
	LocalVector<RDD::PipelineShader> shaders;
	LocalVector<uint32_t> raygen_indices;
	LocalVector<uint32_t> miss_indices;
	LocalVector<RDD::HitGroup> hit_groups;

	SyntheticGroups() {
		const RDD::ShaderStage stages[] = {
			RDD::SHADER_STAGE_RAYGEN,
			RDD::SHADER_STAGE_RAYGEN,
			RDD::SHADER_STAGE_MISS,
			RDD::SHADER_STAGE_CLOSEST_HIT,
			RDD::SHADER_STAGE_ANY_HIT,
			RDD::SHADER_STAGE_INTERSECTION,
		};
		for (uint32_t i = 0; i < std::size(stages); i++) {
			RDD::PipelineShader shader;
			shader.shader = RDD::ShaderID(uint64_t(i + 1));
			shader.shader_stage = stages[i];
			shaders.push_back(shader);
		}

		raygen_indices.push_back(0);
		raygen_indices.push_back(1);
		miss_indices.push_back(2);

		RDD::HitGroup triangle;
		triangle.closest_hit_shader_index = 3;
		triangle.any_hit_shader_index = 4;
		hit_groups.push_back(triangle);

		RDD::HitGroup procedural;
		procedural.closest_hit_shader_index = 3;
		procedural.intersection_shader_index = 5;
		hit_groups.push_back(procedural);

		// Empty hit groups are intentional sentinels in the path-tracer SBT.
		hit_groups.push_back(RDD::HitGroup());
	}
};

static bool configure(MDRaytracingPipeline &r_pipeline, SyntheticGroups &p_groups, String *r_error = nullptr) {
	return r_pipeline.configure_shader_groups(p_groups.shaders, p_groups.raygen_indices, p_groups.miss_indices, p_groups.hit_groups, 2, r_error);
}

TEST_CASE("[MetalRT] C9 maps shader groups in stable bind order") {
	SyntheticGroups input;
	MDRaytracingPipeline pipeline;
	String error;
	REQUIRE_MESSAGE(configure(pipeline, input, &error), error);

	CHECK(pipeline.raygen_group_count == 2);
	CHECK(pipeline.miss_group_count == 1);
	CHECK(pipeline.hit_group_count == 3);
	CHECK(pipeline.max_trace_recursion_depth == 2);
	REQUIRE(pipeline.shader_groups.size() == 6);

	// Godot's bind/SBT order is raygen, miss, then hit. This ordering is the
	// compatibility contract used by RenderingDevice's index offsets.
	CHECK(pipeline.shader_groups[0].type == MDRaytracingPipeline::ShaderGroupType::RAYGEN);
	CHECK(pipeline.shader_groups[1].type == MDRaytracingPipeline::ShaderGroupType::RAYGEN);
	CHECK(pipeline.shader_groups[2].type == MDRaytracingPipeline::ShaderGroupType::MISS);
	CHECK(pipeline.shader_groups[3].type == MDRaytracingPipeline::ShaderGroupType::TRIANGLE_HIT);
	CHECK(pipeline.shader_groups[4].type == MDRaytracingPipeline::ShaderGroupType::PROCEDURAL_HIT);
	CHECK(pipeline.shader_groups[5].type == MDRaytracingPipeline::ShaderGroupType::EMPTY_HIT);

	REQUIRE(pipeline.intersection_functions.size() == 2);
	CHECK(pipeline.intersection_functions[0].type == MDRaytracingPipeline::IntersectionFunctionType::OPAQUE_TRIANGLE);
	CHECK(pipeline.intersection_functions[0].shader_index == UINT32_MAX);
	CHECK(pipeline.intersection_functions[1].type == MDRaytracingPipeline::IntersectionFunctionType::PROCEDURAL);
	CHECK(pipeline.intersection_functions[1].shader_index == 5);
	CHECK(pipeline.shader_groups[3].intersection_function_table_index == 0);
	CHECK(pipeline.shader_groups[4].intersection_function_table_index == 1);
	CHECK(pipeline.shader_groups[5].intersection_function_table_index == UINT32_MAX);
}

TEST_CASE("[MetalRT] C9 emits deterministic shader-group handles") {
	SyntheticGroups input;
	MDRaytracingPipeline pipeline;
	REQUIRE(configure(pipeline, input));

	constexpr uint32_t STRIDE = 32;
	uint8_t bytes[STRIDE * 3];
	memset(bytes, 0xcc, sizeof(bytes));
	const uint32_t relative_hit_indices[] = { 1, 0, 2 };
	String error;
	REQUIRE_MESSAGE(pipeline.get_shader_group_handles(3, VectorView<uint32_t>(relative_hit_indices, 3), bytes, STRIDE, &error), error);

	const auto *procedural = reinterpret_cast<const MDRaytracingPipeline::ShaderGroupHandle *>(bytes);
	const auto *triangle = reinterpret_cast<const MDRaytracingPipeline::ShaderGroupHandle *>(bytes + STRIDE);
	const auto *empty = reinterpret_cast<const MDRaytracingPipeline::ShaderGroupHandle *>(bytes + STRIDE * 2);
	CHECK(procedural->magic == MDRaytracingPipeline::ShaderGroupHandle::MAGIC);
	CHECK(procedural->group_index == 4);
	CHECK(procedural->intersection_function_table_index == 1);
	CHECK(procedural->type == MDRaytracingPipeline::ShaderGroupType::PROCEDURAL_HIT);
	CHECK(triangle->group_index == 3);
	CHECK(triangle->intersection_function_table_index == 0);
	CHECK(empty->group_index == 5);
	CHECK(empty->intersection_function_table_index == UINT32_MAX);

	// Padding in every SBT stride is deterministic, rather than retaining bytes
	// from RenderingDevice's thread-local staging vector.
	for (uint32_t record = 0; record < 3; record++) {
		for (uint32_t i = sizeof(MDRaytracingPipeline::ShaderGroupHandle); i < STRIDE; i++) {
			CHECK(bytes[record * STRIDE + i] == 0);
		}
	}

	CHECK_FALSE(pipeline.get_shader_group_handles(0, VectorView<uint32_t>(relative_hit_indices, 1), bytes, sizeof(MDRaytracingPipeline::ShaderGroupHandle) - 1, &error));
	CHECK(error.contains("stride"));
	const uint32_t invalid_index = 99;
	CHECK_FALSE(pipeline.get_shader_group_handles(0, VectorView<uint32_t>(invalid_index), bytes, STRIDE, &error));
	CHECK(error.contains("out of range"));
}

TEST_CASE("[MetalRT] C9 rejects invalid shader-group mappings") {
	SyntheticGroups input;
	MDRaytracingPipeline pipeline;
	String error;

	CHECK_FALSE(pipeline.configure_shader_groups(input.shaders, VectorView<uint32_t>(), input.miss_indices, input.hit_groups, 2, &error));
	CHECK(error.contains("ray-generation"));

	CHECK_FALSE(pipeline.configure_shader_groups(input.shaders, input.raygen_indices, input.miss_indices, input.hit_groups, 0, &error));
	CHECK(error.contains("recursion"));

	input.shaders[3].shader_stage = RDD::SHADER_STAGE_MISS;
	CHECK_FALSE(configure(pipeline, input, &error));
	CHECK(error.contains("closest-hit"));
}

TEST_CASE_PENDING("[MetalRT][GPU] C9 allocates the mapped Metal function table") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	SyntheticGroups input;
	MDRaytracingPipeline pipeline;
	String error;
	REQUIRE_MESSAGE(configure(pipeline, input, &error), error);
	REQUIRE_MESSAGE(pipeline.create_trace_one_ray(device.get(), &error), error);
	CHECK(pipeline.state);
	CHECK(pipeline.intersection_function_table);
	CHECK(pipeline.intersection_function_count == 2);
	print_line(vformat("MetalRT C9 pipeline mapping: device=\"%s\" groups=%d raygen=%d miss=%d hit=%d ift_entries=%d recursion_budget=%d",
			device->name()->utf8String(), pipeline.shader_groups.size(), pipeline.raygen_group_count, pipeline.miss_group_count, pipeline.hit_group_count, pipeline.intersection_function_count, pipeline.max_trace_recursion_depth));
}

} // namespace TestMetalRTPipeline

#endif // METAL_ENABLED
