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
