/**************************************************************************/
/*  metal_rt_shader_lowering.h                                            */
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

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "servers/rendering/rendering_device_commons.h"

#include <string>

// Shader-lowering shader-lowering lane for ray-tracing SPIR-V.
//
// This is the probe half of the shader strategy documented in
// docs/rt_metal_port/shader_strategy.md: it runs a single SPIR-V stage
// through the same SPIRV-Cross CompilerMSL configuration used by
// RenderingShaderContainerMetal and reports the outcome instead of failing
// the container. It supports every RenderingDeviceCommons::ShaderStage,
// including the ray-tracing pipeline stages the container does not consume
// yet, so tests and future backend code can measure exactly which stages the
// SPIRV-Cross lane can lower.

class MetalRTShaderLowering {
public:
	enum class IntersectorPatchStatus {
		APPLIED,
		NOT_SCENE_TRACE_KERNEL,
		PROCEDURAL_GEOMETRY,
		MISSING_RT_FLAGS,
		INVALID_RT_FLAGS_ORDER,
		TRACE_MATERIAL_LAYOUT,
		TRACE_SHADOW_LAYOUT,
	};

	struct IntersectorPatchResult {
		IntersectorPatchStatus status = IntersectorPatchStatus::NOT_SCENE_TRACE_KERNEL;
		String detail;

		bool applied() const {
			return status == IntersectorPatchStatus::APPLIED;
		}
	};

	// TRAVERSAL_METADATA: explicit traversal-lowering metadata.
	//
	// Every traversal class the compute scene kernel could dispatch through the
	// native metal::raytracing::intersector is declared in a registry (see
	// TRAVERSAL_CLASS_REGISTRY in the implementation) instead of being implied
	// by ad-hoc string rewrites. A class entry names its RT_FLAGS-derived guard
	// constant (per-class pipeline specialization: Metal folds the inactive
	// lane out at pipeline creation), its injection anchors in the SPIRV-Cross
	// output, and — for classes that do not have a native lane yet — the
	// requirements that block them. The engine applies each class
	// transactionally and reports a structured per-class outcome, which the
	// shader container records as reflection metadata for cache validation and
	// pipeline selection.
	enum TraversalClassBits : uint32_t {
		TRAVERSAL_CLASS_NONE = 0,
		// ALL_OPAQUE pipelines: closest-hit and shadow traversal with forced
		// opacity. Guarded by RT_FLAG_ALL_OPAQUE (RT_FLAGS bit 4).
		TRAVERSAL_CLASS_OPAQUE_TRIANGLES = 1 << 0,
		// Alpha-tested triangles. Requires the intersection-function-table
		// path: candidates must run the material alpha test as a Metal
		// intersection function (linked-function compute pipelines plus
		// per-slot alpha evaluators from the aggregate kernel).
		TRAVERSAL_CLASS_ALPHA_TRIANGLES = 1 << 1,
		// Procedural (AABB) geometry. Requires bounding-box intersection
		// functions in an intersection function table.
		TRAVERSAL_CLASS_PROCEDURAL = 1 << 2,
	};

	enum class TraversalClassStatus {
		APPLIED, // The class's native lane was injected.
		NOT_IMPLEMENTED, // Declared in the registry; native lane pending.
		EXCLUDED, // A registry exclusion matched (e.g. procedural content vetoes the triangle-only opaque body).
		ANCHOR_MISMATCH, // The SPIRV-Cross output drifted from the declared anchor.
	};

	struct TraversalClassOutcome {
		uint32_t class_bit = TRAVERSAL_CLASS_NONE;
		TraversalClassStatus status = TraversalClassStatus::NOT_IMPLEMENTED;
		String detail;
	};

	struct TraversalLoweringResult {
		// Shared kernel prerequisites (trace anchors, RT_FLAGS declaration).
		IntersectorPatchStatus kernel_status = IntersectorPatchStatus::NOT_SCENE_TRACE_KERNEL;
		String kernel_detail;
		uint32_t applied_class_mask = TRAVERSAL_CLASS_NONE; // Lanes injected into the MSL.
		uint32_t eligible_class_mask = TRAVERSAL_CLASS_NONE; // Lanes whose prerequisites held.
		Vector<TraversalClassOutcome> classes;

		bool any_applied() const { return applied_class_mask != TRAVERSAL_CLASS_NONE; }
	};

	// Metadata-driven lowering over the registry. With p_apply=false the
	// source is only probed (nothing is modified); the result still reports
	// which classes were eligible, which the container records so cached MSL
	// compiled for the other traversal lane can be rejected without re-parsing.
	static TraversalLoweringResult apply_traversal_lowering(std::string &p_source, bool p_apply = true);
	static const char *traversal_class_name(uint32_t p_class_bit);
	static const char *traversal_class_status_name(TraversalClassStatus p_status);

	struct Result {
		bool ok = false;
		String error;
		String entry_point;
		String msl_source;
	};

	// Lowers one SPIR-V stage to MSL.
	//
	// Every reflected resource is identity-mapped to the MSL index equal to
	// its SPIR-V binding, so callers can bind resources deterministically
	// without the full container reflection pipeline. Acceleration structures
	// count as buffer bindings.
	static Result lower_spirv(RenderingDeviceCommons::ShaderStage p_stage, const Vector<uint8_t> &p_spirv,
			uint32_t p_msl_major, uint32_t p_msl_minor, bool p_argument_buffers, bool p_pad_argument_buffer_resources = true);

	// Rewrites SPIRV-Cross's scene ray-query helpers to dispatch ALL_OPAQUE
	// specializations through metal::raytracing::intersector. The operation is
	// transactional: on any exclusion or anchor mismatch, p_source is unchanged
	// and the result identifies the exact fallback class.
	static IntersectorPatchResult patch_scene_ray_query_to_intersector(std::string &p_source);
	static const char *intersector_patch_status_name(IntersectorPatchStatus p_status);
};
