/**************************************************************************/
/*  metal_rt_pipeline.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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

#include "metal_objects_shared.h"

bool MDRaytracingPipeline::configure_shader_groups(VectorView<RDD::PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<RDD::HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, String *r_error) {
	shader_groups.clear();
	intersection_functions.clear();
	raygen_group_count = 0;
	miss_group_count = 0;
	hit_group_count = 0;
	max_trace_recursion_depth = 0;
	uses_compute_lane = false;
	if (r_error != nullptr) {
		r_error->clear();
	}

	auto fail = [r_error](const String &p_message) {
		if (r_error != nullptr) {
			*r_error = p_message;
		}
		return false;
	};
	auto valid_stage = [&](uint32_t p_shader_index, RDD::ShaderStage p_stage) {
		return p_shader_index < p_shaders.size() && p_shaders[p_shader_index].shader && p_shaders[p_shader_index].shader_stage == p_stage;
	};

	if (p_raygen_shader_indices.size() == 0) {
		return fail("A Metal raytracing pipeline requires at least one ray-generation group.");
	}
	if (p_max_trace_recursion_depth == 0) {
		return fail("The Metal raytracing software recursion budget must be greater than zero.");
	}

	// C10 compute lane: the ray-generation group may be a re-expressed
	// ray-query compute kernel instead of an RT-pipeline stage. That kernel is
	// monolithic — raygen, miss, and hit logic are inlined — so it must be the
	// only shader in the pipeline; miss and hit groups are retained purely as
	// stable SBT records.
	const bool compute_lane = p_raygen_shader_indices[0] < p_shaders.size() && p_shaders[p_raygen_shader_indices[0]].shader_stage == RDD::SHADER_STAGE_COMPUTE;
	if (compute_lane) {
		if (p_raygen_shader_indices.size() != 1) {
			return fail("A Metal compute-lane raytracing pipeline requires exactly one ray-generation group (the re-expressed kernel is monolithic).");
		}
		if (p_miss_shader_indices.size() != 0) {
			return fail("A Metal compute-lane raytracing pipeline cannot reference miss shaders; miss logic is inlined in the re-expressed kernel.");
		}
		for (uint32_t i = 0; i < p_hit_groups.size(); i++) {
			const RDD::HitGroup &input = p_hit_groups[i];
			if (input.closest_hit_shader_index != UINT32_MAX || input.any_hit_shader_index != UINT32_MAX || input.intersection_shader_index != UINT32_MAX) {
				return fail(vformat("Hit group %d references shader stages, but the Metal compute lane inlines hit logic; only empty (sentinel) hit groups are accepted.", i));
			}
		}
	}

	// Slot zero is Metal's system opaque-triangle intersection function. Every
	// triangle hit group can share it because hit/any-hit logic is inlined by the
	// compute lowering rather than represented by independent visible functions.
	IntersectionFunction opaque_triangle;
	opaque_triangle.type = IntersectionFunctionType::OPAQUE_TRIANGLE;
	intersection_functions.push_back(opaque_triangle);

	for (uint32_t i = 0; i < p_raygen_shader_indices.size(); i++) {
		const uint32_t shader_index = p_raygen_shader_indices[i];
		if (!valid_stage(shader_index, compute_lane ? RDD::SHADER_STAGE_COMPUTE : RDD::SHADER_STAGE_RAYGEN)) {
			return fail(vformat("Ray-generation group %d references an invalid shader stage index (%d).", i, shader_index));
		}
		ShaderGroup group;
		group.type = ShaderGroupType::RAYGEN;
		group.general_shader_index = shader_index;
		shader_groups.push_back(group);
	}
	raygen_group_count = p_raygen_shader_indices.size();

	for (uint32_t i = 0; i < p_miss_shader_indices.size(); i++) {
		const uint32_t shader_index = p_miss_shader_indices[i];
		if (!valid_stage(shader_index, RDD::SHADER_STAGE_MISS)) {
			return fail(vformat("Miss group %d references an invalid shader stage index (%d).", i, shader_index));
		}
		ShaderGroup group;
		group.type = ShaderGroupType::MISS;
		group.general_shader_index = shader_index;
		shader_groups.push_back(group);
	}
	miss_group_count = p_miss_shader_indices.size();
	uses_compute_lane = compute_lane;

	for (uint32_t i = 0; i < p_hit_groups.size(); i++) {
		const RDD::HitGroup &input = p_hit_groups[i];
		ShaderGroup group;
		group.closest_hit_shader_index = input.closest_hit_shader_index;
		group.any_hit_shader_index = input.any_hit_shader_index;
		group.intersection_shader_index = input.intersection_shader_index;

		if (input.closest_hit_shader_index != UINT32_MAX && !valid_stage(input.closest_hit_shader_index, RDD::SHADER_STAGE_CLOSEST_HIT)) {
			return fail(vformat("Hit group %d references an invalid closest-hit shader stage index (%d).", i, input.closest_hit_shader_index));
		}
		if (input.any_hit_shader_index != UINT32_MAX && !valid_stage(input.any_hit_shader_index, RDD::SHADER_STAGE_ANY_HIT)) {
			return fail(vformat("Hit group %d references an invalid any-hit shader stage index (%d).", i, input.any_hit_shader_index));
		}
		if (input.intersection_shader_index != UINT32_MAX && !valid_stage(input.intersection_shader_index, RDD::SHADER_STAGE_INTERSECTION)) {
			return fail(vformat("Hit group %d references an invalid intersection shader stage index (%d).", i, input.intersection_shader_index));
		}

		const bool empty = input.closest_hit_shader_index == UINT32_MAX && input.any_hit_shader_index == UINT32_MAX && input.intersection_shader_index == UINT32_MAX;
		if (empty) {
			group.type = ShaderGroupType::EMPTY_HIT;
		} else if (input.intersection_shader_index == UINT32_MAX) {
			group.type = ShaderGroupType::TRIANGLE_HIT;
			group.intersection_function_table_index = 0;
		} else {
			group.type = ShaderGroupType::PROCEDURAL_HIT;
			group.intersection_function_table_index = intersection_functions.size();
			IntersectionFunction procedural;
			procedural.type = IntersectionFunctionType::PROCEDURAL;
			procedural.shader_index = input.intersection_shader_index;
			intersection_functions.push_back(procedural);
		}
		shader_groups.push_back(group);
	}
	hit_group_count = p_hit_groups.size();
	max_trace_recursion_depth = p_max_trace_recursion_depth;
	return true;
}

bool MDRaytracingPipeline::create_compute_lane(MTL::Device *p_device, MTL::Function *p_function, MTL::Size p_local, String *r_error) {
	state.reset();
	intersection_function_table.reset();
	intersection_function_count = 0;
	if (r_error != nullptr) {
		r_error->clear();
	}

	auto fail = [r_error](const String &p_message) {
		if (r_error != nullptr) {
			*r_error = p_message;
		}
		return false;
	};

	if (!uses_compute_lane) {
		return fail("The Metal raytracing pipeline groups were not configured for the compute lane.");
	}
	if (p_device == nullptr || !p_device->supportsRaytracing()) {
		return fail("Metal ray tracing is not supported by this device.");
	}
	if (p_function == nullptr) {
		return fail("The Metal compute-lane kernel function is not valid.");
	}
	if (p_local.width == 0 || p_local.height == 0 || p_local.depth == 0) {
		return fail("The Metal compute-lane workgroup size must be non-zero in every dimension.");
	}

	NS::Error *error = nullptr;
	state = NS::TransferPtr(p_device->newComputePipelineState(p_function, &error));
	if (!state) {
		return fail(error != nullptr ? String::utf8(error->localizedDescription()->utf8String()) : String("Unknown Metal compute-pipeline creation error."));
	}
	threads_per_threadgroup = p_local;

	// Ray-query kernels step candidates through metal::raytracing::
	// intersection_query; no intersection-function table is bound. Procedural
	// hit groups therefore remain rejected on this lane until visible-function
	// lowering exists (docs/rt_metal_port/pathtracer_launch.md).
	return true;
}

bool MDRaytracingPipeline::get_shader_group_handles(uint32_t p_group_index_offset, VectorView<uint32_t> p_group_indices, uint8_t *r_data, uint32_t p_data_stride_bytes, String *r_error) const {
	if (r_error != nullptr) {
		r_error->clear();
	}
	auto fail = [r_error](const String &p_message) {
		if (r_error != nullptr) {
			*r_error = p_message;
		}
		return false;
	};

	if (p_data_stride_bytes < SHADER_GROUP_HANDLE_SIZE || (p_data_stride_bytes % SHADER_GROUP_HANDLE_ALIGNMENT) != 0) {
		return fail(vformat("Metal shader-group data stride (%d) must be at least %d bytes and aligned to %d bytes.", p_data_stride_bytes, SHADER_GROUP_HANDLE_SIZE, SHADER_GROUP_HANDLE_ALIGNMENT));
	}
	if (p_group_indices.size() > 0 && r_data == nullptr) {
		return fail("Metal shader-group handle output is null.");
	}

	for (uint32_t i = 0; i < p_group_indices.size(); i++) {
		const uint64_t group_index_64 = uint64_t(p_group_index_offset) + p_group_indices[i];
		if (group_index_64 >= uint64_t(shader_groups.size())) {
			return fail(vformat("Metal shader-group index (%d + %d) is out of range.", p_group_index_offset, p_group_indices[i]));
		}
		const uint32_t group_index = uint32_t(group_index_64);
		const ShaderGroup &group = shader_groups[group_index];
		ShaderGroupHandle handle;
		handle.group_index = group_index;
		handle.intersection_function_table_index = group.intersection_function_table_index;
		handle.type = group.type;

		memset(r_data, 0, p_data_stride_bytes);
		memcpy(r_data, &handle, sizeof(handle));
		r_data += p_data_stride_bytes;
	}
	return true;
}
