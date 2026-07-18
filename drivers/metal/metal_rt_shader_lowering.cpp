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

static bool _msl_has_function_injection_layout(const std::string &p_source, const char *p_signature_start) {
	size_t def_pos = p_source.find(p_signature_start);
	if (def_pos == std::string::npos) {
		return false;
	}
	size_t brace = p_source.find('{', def_pos);
	return brace >= 2 && brace + 1 < p_source.size() &&
			p_source.compare(brace - 2, 4, ")\n{\n") == 0;
}

// SPIRV-Cross threads every resource used by a helper through its generated
// MSL signature. Extract the parameter names from the public wrapper so an
// injected call to its masked query implementation forwards those resources
// without depending on SPIRV-Cross's generated numeric identifiers.
static bool _msl_get_forwarded_parameters(const std::string &p_source, const char *p_signature_start, uint32_t p_skip_count, std::string &r_parameters) {
	size_t def_pos = p_source.find(p_signature_start);
	if (def_pos == std::string::npos) {
		return false;
	}
	size_t open = p_source.find('(', def_pos);
	if (open == std::string::npos) {
		return false;
	}

	LocalVector<std::string> parameters;
	size_t parameter_start = open + 1;
	int nesting = 0;
	for (size_t i = parameter_start; i < p_source.size(); i++) {
		const char c = p_source[i];
		if (c == '<' || c == '(' || c == '[') {
			nesting++;
		} else if (c == '>' || c == ']') {
			nesting--;
		} else if (c == ')' && nesting == 0) {
			parameters.push_back(p_source.substr(parameter_start, i - parameter_start));
			break;
		} else if (c == ')') {
			nesting--;
		} else if (c == ',' && nesting == 0) {
			parameters.push_back(p_source.substr(parameter_start, i - parameter_start));
			parameter_start = i + 1;
		}
	}
	if (parameters.size() < p_skip_count) {
		return false;
	}

	r_parameters.clear();
	for (uint32_t i = p_skip_count; i < parameters.size(); i++) {
		const std::string &parameter = parameters[i];
		size_t end = parameter.find_last_not_of(" \t\r\n");
		if (end == std::string::npos) {
			return false;
		}
		size_t begin = end;
		while (begin > 0 && ((parameter[begin - 1] >= 'a' && parameter[begin - 1] <= 'z') ||
					(parameter[begin - 1] >= 'A' && parameter[begin - 1] <= 'Z') ||
					(parameter[begin - 1] >= '0' && parameter[begin - 1] <= '9') || parameter[begin - 1] == '_')) {
			begin--;
		}
		if (begin > end || !((parameter[begin] >= 'a' && parameter[begin] <= 'z') ||
					(parameter[begin] >= 'A' && parameter[begin] <= 'Z') || parameter[begin] == '_')) {
			return false;
		}
		r_parameters += ", " + parameter.substr(begin, end - begin + 1);
	}
	return true;
}

static bool _msl_expand_forwarded_parameters(const std::string &p_source, const char *p_signature_start, std::string &r_dispatch_body) {
	static constexpr char marker[] = "$GODOT_FORWARDED_PARAMETERS";
	size_t marker_pos = r_dispatch_body.find(marker);
	if (marker_pos == std::string::npos) {
		return true;
	}
	const uint32_t fixed_parameter_count = strcmp(p_signature_start, "bool trace_material(") == 0 ? 4 : 3;
	std::string forwarded;
	if (!_msl_get_forwarded_parameters(p_source, p_signature_start, fixed_parameter_count, forwarded)) {
		return false;
	}
	r_dispatch_body.replace(marker_pos, strlen(marker), forwarded);
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

// --- TRAVERSAL_METADATA: traversal-class registry -----------------------------------------
//
// Declarative metadata for every traversal class the compute scene kernel can
// dispatch natively. The engine below is generic; broadening intersector
// coverage means adding or completing a registry entry, not growing another
// textual rewrite.

namespace {

struct TraversalInjection {
	const char *anchor_signature; // Function definition the dispatch is injected into.
	const char *dispatch_body; // Injected immediately after the opening brace.
	MetalRTShaderLowering::IntersectorPatchStatus mismatch_status; // Legacy status for anchor drift.
};

struct TraversalClassDesc {
	uint32_t class_bit;
	const char *name;
	// Nonnull: a symbol whose presence in the variant vetoes this class (the
	// legacy status names the veto for the compatibility wrapper).
	const char *exclude_if_present;
	const char *exclusion_detail;
	// Guard + native helper functions inserted once before the first anchor.
	// Null helpers = the class has no native lane yet; `pending_requirements`
	// documents what unblocks it.
	const char *helpers;
	const TraversalInjection *injections;
	uint32_t injection_count;
	const char *pending_requirements;
};

constexpr char OPAQUE_TRIANGLES_HELPERS[] = R"(
// --- Godot: native-intersector fast paths for opaque TLAS partitions -------
constant bool godot_use_intersector = ((RT_FLAGS & 16u) != 0u); // RT_FLAG_ALL_OPAQUE
constant bool godot_use_mixed_intersector = ((RT_FLAGS & 32u) != 0u); // RT_FLAG_MIXED_ALPHA

static bool godot_trace_material_intersector(const thread float3 &origin, const thread float3 &direction, float max_distance, uint instance_mask, thread ComputeHit &hit, raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    raytracing::ray r(origin, direction, 0.001, max_distance);
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> trace;
    trace.assume_geometry_type(raytracing::geometry_type::triangle);
    trace.force_opacity(raytracing::forced_opacity::opaque);
    trace.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
    trace.accept_any_intersection(false);
    auto result = trace.intersect(r, tlas, instance_mask);
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

static bool godot_trace_shadow_blocked_intersector(const thread float3 &origin, const thread float3 &direction, float max_distance, uint instance_mask, raytracing::acceleration_structure<raytracing::instancing> tlas)
{
    raytracing::ray r(origin, direction, 0.001, max_distance);
    raytracing::intersector<raytracing::instancing, raytracing::triangle_data> trace;
    trace.assume_geometry_type(raytracing::geometry_type::triangle);
    trace.force_opacity(raytracing::forced_opacity::opaque);
    trace.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
    trace.accept_any_intersection(true);
    auto result = trace.intersect(r, tlas, instance_mask);
    return result.type != raytracing::intersection_type::none;
}

)";

constexpr TraversalInjection OPAQUE_TRIANGLES_INJECTIONS[] = {
	{
			"bool trace_material(",
			"    if (godot_use_intersector)\n"
			"    {\n"
			"        return godot_trace_material_intersector(origin, direction, max_distance, 0x01u, hit, tlas);\n"
			"    }\n"
			"    if (godot_use_mixed_intersector)\n"
			"    {\n"
			"        bool opaque_hit = godot_trace_material_intersector(origin, direction, max_distance, 0x01u, hit, tlas);\n"
			"        float query_max_distance = opaque_hit ? hit.t : max_distance;\n"
			"        ComputeHit query_hit = {};\n"
			"        if (trace_material_query(origin, direction, query_max_distance, query_hit, 0x02u$GODOT_FORWARDED_PARAMETERS))\n"
			"        {\n"
			"            hit = query_hit;\n"
			"            return true;\n"
			"        }\n"
			"        return opaque_hit;\n"
			"    }\n",
			MetalRTShaderLowering::IntersectorPatchStatus::TRACE_MATERIAL_LAYOUT,
	},
	{
			"bool trace_shadow_blocked(",
			"    if (godot_use_intersector)\n"
			"    {\n"
			"        return godot_trace_shadow_blocked_intersector(origin, direction, max_distance, 0x01u, tlas);\n"
			"    }\n"
			"    if (godot_use_mixed_intersector)\n"
			"    {\n"
			"        if (godot_trace_shadow_blocked_intersector(origin, direction, max_distance, 0x01u, tlas))\n"
			"        {\n"
			"            return true;\n"
			"        }\n"
			"        return trace_shadow_blocked_query(origin, direction, max_distance, 0x02u$GODOT_FORWARDED_PARAMETERS);\n"
			"    }\n",
			MetalRTShaderLowering::IntersectorPatchStatus::TRACE_SHADOW_LAYOUT,
	},
};

constexpr TraversalClassDesc TRAVERSAL_CLASS_REGISTRY[] = {
	{
			MetalRTShaderLowering::TRAVERSAL_CLASS_OPAQUE_TRIANGLES,
			"opaque_triangles",
			nullptr,
			nullptr,
			OPAQUE_TRIANGLES_HELPERS,
			OPAQUE_TRIANGLES_INJECTIONS,
			(uint32_t)(sizeof(OPAQUE_TRIANGLES_INJECTIONS) / sizeof(OPAQUE_TRIANGLES_INJECTIONS[0])),
			nullptr,
	},
	{
			MetalRTShaderLowering::TRAVERSAL_CLASS_ALPHA_TRIANGLES,
			"alpha_triangles",
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			0,
			"intersection-function-table lane: alpha candidates must run the material alpha test as a Metal intersection function (linked-function compute pipelines plus per-slot alpha evaluators from the aggregate kernel), guarded by a dedicated RT_FLAGS bit",
	},
	{
			MetalRTShaderLowering::TRAVERSAL_CLASS_PROCEDURAL,
			"procedural",
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			0,
			"bounding-box intersection functions in an intersection function table; depends on the alpha_triangles linked-function infrastructure",
	},
};

} // namespace

const char *MetalRTShaderLowering::traversal_class_name(uint32_t p_class_bit) {
	for (const TraversalClassDesc &desc : TRAVERSAL_CLASS_REGISTRY) {
		if (desc.class_bit == p_class_bit) {
			return desc.name;
		}
	}
	return "unknown";
}

const char *MetalRTShaderLowering::traversal_class_status_name(TraversalClassStatus p_status) {
	switch (p_status) {
		case TraversalClassStatus::APPLIED:
			return "applied";
		case TraversalClassStatus::NOT_IMPLEMENTED:
			return "not_implemented";
		case TraversalClassStatus::EXCLUDED:
			return "excluded";
		case TraversalClassStatus::ANCHOR_MISMATCH:
			return "anchor_mismatch";
	}
	return "unknown";
}

MetalRTShaderLowering::TraversalLoweringResult MetalRTShaderLowering::apply_traversal_lowering(std::string &p_source, bool p_apply) {
	TraversalLoweringResult result;

	// Shared kernel prerequisites: the scene trace anchors and the RT_FLAGS
	// specialization constant every class guard folds on.
	if (p_source.find("bool trace_material(") == std::string::npos ||
			p_source.find("bool trace_shadow_blocked(") == std::string::npos) {
		result.kernel_status = IntersectorPatchStatus::NOT_SCENE_TRACE_KERNEL;
		result.kernel_detail = "missing trace_material/trace_shadow_blocked anchors (not the scene trace kernel, or SPIRV-Cross output drifted)";
		return result;
	}
	size_t rt_flags_pos = p_source.find("constant uint RT_FLAGS ");
	if (rt_flags_pos == std::string::npos) {
		result.kernel_status = IntersectorPatchStatus::MISSING_RT_FLAGS;
		result.kernel_detail = "missing RT_FLAGS function-constant anchor";
		return result;
	}
	if (rt_flags_pos > p_source.find("bool trace_material(")) {
		result.kernel_status = IntersectorPatchStatus::INVALID_RT_FLAGS_ORDER;
		result.kernel_detail = "RT_FLAGS constant is not declared before trace_material";
		return result;
	}
	result.kernel_status = IntersectorPatchStatus::APPLIED;
	result.kernel_detail = "kernel prerequisites hold";

	// Probe-only calls are on the query lane. Avoid copying and injecting the
	// full generated MSL merely to discard it after collecting eligibility.
	std::string source;
	if (p_apply) {
		source = p_source;
	}
	bool modified = false;

	for (const TraversalClassDesc &desc : TRAVERSAL_CLASS_REGISTRY) {
		TraversalClassOutcome outcome;
		outcome.class_bit = desc.class_bit;

		if (desc.helpers == nullptr) {
			outcome.status = TraversalClassStatus::NOT_IMPLEMENTED;
			outcome.detail = String("pending: ") + desc.pending_requirements;
			result.classes.push_back(outcome);
			continue;
		}
		if (desc.exclude_if_present != nullptr && p_source.find(desc.exclude_if_present) != std::string::npos) {
			outcome.status = TraversalClassStatus::EXCLUDED;
			outcome.detail = desc.exclusion_detail;
			result.classes.push_back(outcome);
			continue;
		}

		if (!p_apply) {
			bool eligible = true;
			for (uint32_t i = 0; i < desc.injection_count; i++) {
				if (!_msl_has_function_injection_layout(p_source, desc.injections[i].anchor_signature)) {
					outcome.status = TraversalClassStatus::ANCHOR_MISMATCH;
					outcome.detail = String(desc.injections[i].anchor_signature) + " did not match the expected SPIRV-Cross layout";
					eligible = false;
					break;
				}
			}
			if (eligible) {
				result.eligible_class_mask |= desc.class_bit;
				outcome.status = TraversalClassStatus::APPLIED;
				outcome.detail = String(desc.name) + " native lane eligible (probe only)";
			}
			result.classes.push_back(outcome);
			continue;
		}

		// Transactional per-class application against a scratch copy.
		std::string trial = source;
		size_t first_anchor = trial.find(desc.injections[0].anchor_signature);
		bool applied = first_anchor != std::string::npos;
		if (applied) {
			trial.insert(_msl_function_insert_pos(trial, first_anchor), desc.helpers);
			for (uint32_t i = 0; i < desc.injection_count; i++) {
				std::string dispatch_body = desc.injections[i].dispatch_body;
				if (!_msl_expand_forwarded_parameters(trial, desc.injections[i].anchor_signature, dispatch_body) ||
						!_msl_inject_after_function_brace(trial, desc.injections[i].anchor_signature, dispatch_body.c_str())) {
					outcome.status = TraversalClassStatus::ANCHOR_MISMATCH;
					outcome.detail = String(desc.injections[i].anchor_signature) + " did not match the expected SPIRV-Cross layout";
					applied = false;
					break;
				}
			}
		} else {
			outcome.status = TraversalClassStatus::ANCHOR_MISMATCH;
			outcome.detail = String(desc.injections[0].anchor_signature) + " anchor not found";
		}

		if (applied) {
			source = std::move(trial);
			modified = true;
			result.applied_class_mask |= desc.class_bit;
			result.eligible_class_mask |= desc.class_bit;
			outcome.status = TraversalClassStatus::APPLIED;
			outcome.detail = String(desc.name) + " native lane injected";
		}
		result.classes.push_back(outcome);
	}

	if (p_apply && modified) {
		p_source = std::move(source);
	}
	return result;
}

MetalRTShaderLowering::IntersectorPatchResult MetalRTShaderLowering::patch_scene_ray_query_to_intersector(std::string &p_source) {
	// Compatibility facade over the traversal-class registry: reports the
	// opaque-triangle class outcome using the original status vocabulary.
	IntersectorPatchResult result;
	TraversalLoweringResult lowering = apply_traversal_lowering(p_source, /*p_apply=*/true);
	if (lowering.kernel_status != IntersectorPatchStatus::APPLIED) {
		result.status = lowering.kernel_status;
		result.detail = lowering.kernel_detail;
		return result;
	}
	for (const TraversalClassOutcome &outcome : lowering.classes) {
		if (outcome.class_bit != TRAVERSAL_CLASS_OPAQUE_TRIANGLES) {
			continue;
		}
		switch (outcome.status) {
			case TraversalClassStatus::APPLIED:
				result.status = IntersectorPatchStatus::APPLIED;
				result.detail = "ALL_OPAQUE closest-hit and shadow helpers injected";
				break;
			case TraversalClassStatus::EXCLUDED:
				result.status = IntersectorPatchStatus::PROCEDURAL_GEOMETRY;
				result.detail = outcome.detail;
				break;
			case TraversalClassStatus::ANCHOR_MISMATCH: {
				// Map the failing anchor back to the original status codes.
				result.status = outcome.detail.begins_with("bool trace_shadow_blocked(")
						? IntersectorPatchStatus::TRACE_SHADOW_LAYOUT
						: IntersectorPatchStatus::TRACE_MATERIAL_LAYOUT;
				result.detail = outcome.detail;
			} break;
			case TraversalClassStatus::NOT_IMPLEMENTED:
				result.status = IntersectorPatchStatus::NOT_SCENE_TRACE_KERNEL;
				result.detail = outcome.detail;
				break;
		}
		return result;
	}
	result.status = IntersectorPatchStatus::NOT_SCENE_TRACE_KERNEL;
	result.detail = "opaque_triangles class missing from the traversal registry";
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
		// enabled; SHADER_LOWERING measures both configurations.
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
