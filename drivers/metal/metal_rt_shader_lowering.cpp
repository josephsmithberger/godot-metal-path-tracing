/**************************************************************************/
/*  metal_rt_shader_lowering.cpp                                          */
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

#include "metal_rt_shader_lowering.h"

#include <spirv.hpp>
#include <spirv_msl.hpp>
#include <spirv_parser.hpp>

#include <cstring>

static size_t _msl_function_insert_pos(const std::string &p_source, size_t p_def_pos) {
	size_t line_start = p_source.rfind('\n', p_def_pos);
	line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
	if (line_start >= 2) {
		size_t prev_line_start = p_source.rfind('\n', line_start - 2);
		prev_line_start = (prev_line_start == std::string::npos) ? 0 : prev_line_start + 1;
		if (p_source.compare(prev_line_start, strlen("static"), "static") == 0 &&
				p_source.find("__attribute__", prev_line_start) < line_start) {
			return prev_line_start;
		}
	}
	return line_start;
}

static bool _msl_inject_after_function_brace(std::string &r_source, const char *p_signature_start, const char *p_block) {
	size_t def_pos = r_source.find(p_signature_start);
	if (def_pos == std::string::npos) {
		return false;
	}
	// Inspect the definition's first opening brace. Searching for a later
	// ")\n{\n" can cross into the next function when this layout drifts and
	// inject the material dispatch into the shadow body.
	size_t brace = r_source.find('{', def_pos);
	if (brace < 2 || brace + 1 >= r_source.size() ||
			r_source.compare(brace - 2, 4, ")\n{\n") != 0) {
		return false;
	}
	r_source.insert(brace + 2, p_block);
	return true;
}

const char *MetalRTShaderLowering::intersector_patch_status_name(IntersectorPatchStatus p_status) {
	switch (p_status) {
		case IntersectorPatchStatus::APPLIED:
			return "applied";
		case IntersectorPatchStatus::NOT_SCENE_TRACE_KERNEL:
			return "not_scene_trace_kernel";
		case IntersectorPatchStatus::PROCEDURAL_GEOMETRY:
			return "procedural_geometry";
		case IntersectorPatchStatus::MISSING_RT_FLAGS:
			return "missing_rt_flags";
		case IntersectorPatchStatus::INVALID_RT_FLAGS_ORDER:
			return "invalid_rt_flags_order";
		case IntersectorPatchStatus::TRACE_MATERIAL_LAYOUT:
			return "trace_material_layout";
		case IntersectorPatchStatus::TRACE_SHADOW_LAYOUT:
			return "trace_shadow_layout";
	}
	return "unknown";
}

MetalRTShaderLowering::IntersectorPatchResult MetalRTShaderLowering::patch_scene_ray_query_to_intersector(std::string &p_source) {
	IntersectorPatchResult result;
	if (p_source.find("bool trace_material(") == std::string::npos ||
			p_source.find("bool trace_shadow_blocked(") == std::string::npos) {
		result.status = IntersectorPatchStatus::NOT_SCENE_TRACE_KERNEL;
		result.detail = "missing trace_material/trace_shadow_blocked anchors (not the scene trace kernel, or SPIRV-Cross output drifted)";
		return result;
	}
	if (p_source.find("commit_bounding_box_intersection") != std::string::npos) {
		result.status = IntersectorPatchStatus::PROCEDURAL_GEOMETRY;
		result.detail = "variant commits procedural bounding boxes (intentional exclusion: intersector body is triangle-only)";
		return result;
	}
	size_t rt_flags_pos = p_source.find("constant uint RT_FLAGS ");
	if (rt_flags_pos == std::string::npos) {
		result.status = IntersectorPatchStatus::MISSING_RT_FLAGS;
		result.detail = "missing RT_FLAGS function-constant anchor";
		return result;
	}

	std::string source = p_source;
	static constexpr char trace_helpers[] = R"(
// --- Godot: native-intersector fast path for ALL_OPAQUE pipelines ----------
constant bool godot_use_intersector = ((RT_FLAGS & 16u) != 0u); // RT_FLAG_ALL_OPAQUE

static bool godot_trace_material_intersector(const thread float3 &origin, const thread float3 &direction, float max_distance, thread ComputeHit &hit, raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    raytracing::ray r(origin, direction, 0.001, max_distance);
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> trace;
    trace.assume_geometry_type(raytracing::geometry_type::triangle);
    trace.force_opacity(raytracing::forced_opacity::opaque);
    trace.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
    trace.accept_any_intersection(false);
    auto result = trace.intersect(r, tlas, 0xFFu);
    if (result.type != raytracing::intersection_type::triangle)
    {
        return false;
    }
    hit.t = result.distance;
    hit.geometry_idx = result.user_instance_id;
    hit.primitive_idx = result.primitive_id;
    hit.barycentrics = result.triangle_barycentric_coord;
    hit.front_face = short(result.triangle_front_facing);
    hit.procedural = short(false);
    hit.hit_kind = result.triangle_front_facing ? 254u : 255u;
    return true;
}

static bool godot_trace_shadow_blocked_intersector(const thread float3 &origin, const thread float3 &direction, float max_distance, raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    raytracing::ray r(origin, direction, 0.001, max_distance);
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> trace;
    trace.assume_geometry_type(raytracing::geometry_type::triangle);
    trace.force_opacity(raytracing::forced_opacity::opaque);
    trace.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
    trace.accept_any_intersection(true);
    auto result = trace.intersect(r, tlas, 0xFFu);
    return result.type != raytracing::intersection_type::none;
}

)";
	size_t trace_def = source.find("bool trace_material(");
	if (trace_def == std::string::npos || rt_flags_pos > trace_def) {
		result.status = IntersectorPatchStatus::INVALID_RT_FLAGS_ORDER;
		result.detail = "RT_FLAGS constant is not declared before trace_material";
		return result;
	}
	source.insert(_msl_function_insert_pos(source, trace_def), trace_helpers);

	if (!_msl_inject_after_function_brace(source, "bool trace_material(",
				"    if (godot_use_intersector)\n"
				"    {\n"
				"        return godot_trace_material_intersector(origin, direction, max_distance, hit, tlas);\n"
				"    }\n")) {
		result.status = IntersectorPatchStatus::TRACE_MATERIAL_LAYOUT;
		result.detail = "trace_material opening brace did not match the expected SPIRV-Cross layout";
		return result;
	}
	if (!_msl_inject_after_function_brace(source, "bool trace_shadow_blocked(",
				"    if (godot_use_intersector)\n"
				"    {\n"
				"        return godot_trace_shadow_blocked_intersector(origin, direction, max_distance, tlas);\n"
				"    }\n")) {
		result.status = IntersectorPatchStatus::TRACE_SHADOW_LAYOUT;
		result.detail = "trace_shadow_blocked opening brace did not match the expected SPIRV-Cross layout";
		return result;
	}

	p_source = std::move(source);
	result.status = IntersectorPatchStatus::APPLIED;
	result.detail = "ALL_OPAQUE closest-hit and shadow helpers injected";
	return result;
}

static spv::ExecutionModel stage_to_execution_model(RenderingDeviceCommons::ShaderStage p_stage) {
	switch (p_stage) {
		case RenderingDeviceCommons::SHADER_STAGE_VERTEX:
			return spv::ExecutionModelVertex;
		case RenderingDeviceCommons::SHADER_STAGE_FRAGMENT:
			return spv::ExecutionModelFragment;
		case RenderingDeviceCommons::SHADER_STAGE_TESSELATION_CONTROL:
			return spv::ExecutionModelTessellationControl;
		case RenderingDeviceCommons::SHADER_STAGE_TESSELATION_EVALUATION:
			return spv::ExecutionModelTessellationEvaluation;
		case RenderingDeviceCommons::SHADER_STAGE_COMPUTE:
			return spv::ExecutionModelGLCompute;
		case RenderingDeviceCommons::SHADER_STAGE_RAYGEN:
			return spv::ExecutionModelRayGenerationKHR;
		case RenderingDeviceCommons::SHADER_STAGE_ANY_HIT:
			return spv::ExecutionModelAnyHitKHR;
		case RenderingDeviceCommons::SHADER_STAGE_CLOSEST_HIT:
			return spv::ExecutionModelClosestHitKHR;
		case RenderingDeviceCommons::SHADER_STAGE_MISS:
			return spv::ExecutionModelMissKHR;
		case RenderingDeviceCommons::SHADER_STAGE_INTERSECTION:
			return spv::ExecutionModelIntersectionKHR;
		default:
			return spv::ExecutionModelMax;
	}
}

MetalRTShaderLowering::Result MetalRTShaderLowering::lower_spirv(RenderingDeviceCommons::ShaderStage p_stage, const Vector<uint8_t> &p_spirv,
		uint32_t p_msl_major, uint32_t p_msl_minor, bool p_argument_buffers, bool p_pad_argument_buffer_resources) {
	using namespace spirv_cross;

	Result result;
	if (p_spirv.size() < 20 || (p_spirv.size() % sizeof(uint32_t)) != 0) {
		result.error = "SPIR-V blob is empty or not word-aligned.";
		return result;
	}

	spv::ExecutionModel execution_model = stage_to_execution_model(p_stage);
	if (execution_model == spv::ExecutionModelMax) {
		result.error = "Unknown shader stage.";
		return result;
	}

	try {
		Parser parser(reinterpret_cast<const uint32_t *>(p_spirv.ptr()), p_spirv.size() / sizeof(uint32_t));
		parser.parse();

		CompilerMSL compiler(std::move(parser.get_parsed_ir()));

		// Mirror RenderingShaderContainerMetal::_set_code_from_spirv() for the
		// options that shape RT-relevant codegen.
		CompilerMSL::Options msl_options{};
		msl_options.set_msl_version(p_msl_major, p_msl_minor);
		msl_options.platform = CompilerMSL::Options::macOS;
		msl_options.argument_buffers = p_argument_buffers;
		msl_options.argument_buffers_tier = p_argument_buffers ? CompilerMSL::Options::ArgumentBuffersTier::Tier2 : CompilerMSL::Options::ArgumentBuffersTier::Tier1;
		msl_options.force_active_argument_buffer_resources = p_argument_buffers;
		// The container always pads argument buffer resources, but SPIRV-Cross's
		// binding registration rejects acceleration structures when padding is
		// enabled; C7 measures both configurations.
		msl_options.pad_argument_buffer_resources = p_argument_buffers && p_pad_argument_buffer_resources;
		msl_options.texture_buffer_native = true;
		msl_options.pad_fragment_output_components = true;
		compiler.set_msl_options(msl_options);

		CompilerGLSL::Options options{};
		options.vertex.flip_vert_y = true;
		compiler.set_common_options(options);

		// Identity-map each reflected descriptor-set-0 resource to the MSL
		// index equal to its SPIR-V binding, with the base type SPIRV-Cross
		// requires when padding argument buffers. This mirrors, in miniature,
		// the binding translation RenderingShaderContainerMetal performs; the
		// container itself has no acceleration-structure uniform case yet.
		ShaderResources resources = compiler.get_shader_resources();
		auto add_bindings = [&](const SmallVector<Resource> &p_resources, SPIRType::BaseType p_basetype) {
			for (const Resource &res : p_resources) {
				MSLResourceBinding rb;
				rb.stage = execution_model;
				rb.basetype = p_basetype;
				rb.desc_set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
				rb.binding = compiler.get_decoration(res.id, spv::DecorationBinding);
				rb.count = 1;
				// Runtime-sized (unbounded) arrays keep a count of 0 so SPIRV-Cross
				// lowers them as spvDescriptorArray; that requires the argument
				// buffer of the set to live in the device address space, matching
				// the container configuration for bindless sets.
				const SPIRType &type = compiler.get_type(res.type_id);
				if (!type.array.empty() && type.array_size_literal.front() && type.array.front() == 0) {
					rb.count = 0;
					if (p_argument_buffers) {
						compiler.set_argument_buffer_device_address_space(rb.desc_set, true);
					}
				}
				rb.msl_buffer = rb.binding;
				rb.msl_texture = rb.binding;
				rb.msl_sampler = rb.binding;
				compiler.add_msl_resource_binding(rb);
			}
		};
		add_bindings(resources.acceleration_structures, SPIRType::AccelerationStructure);
		add_bindings(resources.storage_buffers, SPIRType::Void);
		add_bindings(resources.uniform_buffers, SPIRType::Void);
		add_bindings(resources.storage_images, SPIRType::Image);
		add_bindings(resources.separate_images, SPIRType::Image);
		add_bindings(resources.separate_samplers, SPIRType::Sampler);
		add_bindings(resources.sampled_images, SPIRType::SampledImage);

		std::string source = compiler.compile();

		auto entry_points = compiler.get_entry_points_and_stages();
		if (entry_points.size() != 1) {
			result.error = "Expected a single entry point and stage.";
			return result;
		}
		result.entry_point = String(compiler.get_cleansed_entry_point_name(entry_points.front().name, entry_points.front().execution_model).c_str());
		result.msl_source = String(source.c_str());
		result.ok = true;
	} catch (CompilerError &e) {
		result.error = String(e.what());
	}
	return result;
}
