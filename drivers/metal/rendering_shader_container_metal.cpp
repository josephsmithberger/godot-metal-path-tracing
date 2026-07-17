/**************************************************************************/
/*  rendering_shader_container_metal.cpp                                  */
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

#include "rendering_shader_container_metal.h"

#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/os/os.h"
#include "core/templates/fixed_vector.h"
#include "drivers/metal/metal_rt_shader_lowering.h"
#include "drivers/metal/metal_utils.h"

#include <thirdparty/spirv-reflect/spirv_reflect.h>

#include <Metal/Metal.hpp>
#include <spirv.hpp>
#include <spirv_msl.hpp>
#include <spirv_parser.hpp>

void RenderingShaderContainerMetal::_initialize_toolchain_properties() {
	if (compiler_props.is_valid()) {
		return;
	}

	String sdk;
	switch (device_profile->platform) {
		case MetalDeviceProfile::Platform::macOS:
			sdk = "macosx";
			break;
		case MetalDeviceProfile::Platform::iOS:
			sdk = "iphoneos";
			break;
		case MetalDeviceProfile::Platform::visionOS:
			sdk = "xros";
			break;
	}

	Vector<String> parts{ "echo", R"("")", "|", "/usr/bin/xcrun", "-sdk", sdk, "metal", "-E", "-dM", "-x", "metal" };

	switch (device_profile->platform) {
		case MetalDeviceProfile::Platform::macOS: {
			parts.push_back("-mtargetos=macos" + device_profile->min_os_version.to_compiler_os_version());
			break;
		}
		case MetalDeviceProfile::Platform::iOS: {
			parts.push_back("-mtargetos=ios" + device_profile->min_os_version.to_compiler_os_version());
			break;
		}
		case MetalDeviceProfile::Platform::visionOS: {
			parts.push_back("-mtargetos=xros" + device_profile->min_os_version.to_compiler_os_version());
			break;
		}
	}

	parts.append_array({ "-", "|", "grep", "-E", R"(\"__METAL_VERSION__|__ENVIRONMENT_OS\")" });

	List<String> args = { "-c", String(" ").join(parts) };

	String r_pipe;
	int exit_code;
	Error err = OS::get_singleton()->execute("sh", args, &r_pipe, &exit_code, true);
	ERR_FAIL_COND_MSG(err != OK, "Failed to determine Metal toolchain properties");

	// Parse the lines, which are in the form:
	//
	// #define VARNAME VALUE
	Vector<String> lines = r_pipe.split("\n", false);
	for (String &line : lines) {
		Vector<String> name_val = line.trim_prefix("#define ").split(" ");
		if (name_val.size() != 2) {
			continue;
		}
		if (name_val[0] == "__ENVIRONMENT_OS_VERSION_MIN_REQUIRED__") {
			compiler_props.os_version_min_required = MinOsVersion((uint32_t)name_val[1].to_int());
		} else if (name_val[0] == "__METAL_VERSION__") {
			uint32_t ver = (uint32_t)name_val[1].to_int();
			uint32_t maj = ver / 100;
			uint32_t min = (ver % 100) / 10;
			compiler_props.metal_version = make_msl_version(maj, min);
		}

		if (compiler_props.is_valid()) {
			break;
		}
	}
}

Error RenderingShaderContainerMetal::compile_metal_source(const char *p_source, const StageData &p_stage_data, Vector<uint8_t> &r_binary_data) {
	String name(shader_name.ptr());
	if (name.contains_char(':')) {
		name = name.replace_char(':', '_');
	}
	Error r_error;
	Ref<FileAccess> source_file = FileAccess::create_temp(FileAccess::ModeFlags::READ_WRITE,
			name + "_" + itos(p_stage_data.hash.short_sha()),
			"metal", false, &r_error);
	ERR_FAIL_COND_V_MSG(r_error != OK, r_error, "Unable to create temporary source file.");
	if (!source_file->store_buffer((const uint8_t *)p_source, strlen(p_source))) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "Unable to write temporary source file");
	}
	source_file->flush();
	Ref<FileAccess> result_file = FileAccess::create_temp(FileAccess::ModeFlags::READ_WRITE,
			name + "_" + itos(p_stage_data.hash.short_sha()),
			"metallib", false, &r_error);

	ERR_FAIL_COND_V_MSG(r_error != OK, r_error, "Unable to create temporary target file");

	String sdk;
	switch (device_profile->platform) {
		case MetalDeviceProfile::Platform::macOS:
			sdk = "macosx";
			break;
		case MetalDeviceProfile::Platform::iOS:
			sdk = "iphoneos";
			break;
		case MetalDeviceProfile::Platform::visionOS:
			sdk = "xros";
			break;
	}

	// Build the .metallib binary.
	{
		List<String> args{
			"-sdk",
			sdk,
			"metal",
			"-O3",
			"-Wno-unused-variable",
			"-Wno-uninitialized",
			"-Wno-unused-function",
		};

		// Compile metal shaders for the minimum supported target instead of the host machine.
		switch (device_profile->platform) {
			case MetalDeviceProfile::Platform::macOS: {
				args.push_back("-mtargetos=macos" + device_profile->min_os_version.to_compiler_os_version());
				break;
			}
			case MetalDeviceProfile::Platform::iOS: {
				args.push_back("-mtargetos=ios" + device_profile->min_os_version.to_compiler_os_version());
				break;
			}
			case MetalDeviceProfile::Platform::visionOS: {
				args.push_back("-mtargetos=xros" + device_profile->min_os_version.to_compiler_os_version());
				break;
			}
		}

		if (p_stage_data.is_position_invariant) {
			args.push_back("-fpreserve-invariance");
		}
		args.push_back("-fmetal-math-mode=fast");
		args.push_back(source_file->get_path_absolute());
		args.push_back("-o");
		args.push_back(result_file->get_path_absolute());
		String r_pipe;
		int exit_code;
		Error err = OS::get_singleton()->execute("/usr/bin/xcrun", args, &r_pipe, &exit_code, true);
		if (!r_pipe.is_empty()) {
			print_line(r_pipe);
		}
		if (err != OK) {
			ERR_PRINT(vformat("Metal compiler returned error code: %d", err));
		}

		if (exit_code != 0) {
			ERR_PRINT(vformat("Metal compiler exited with error code: %d", exit_code));
		}
		int len = result_file->get_length();
		ERR_FAIL_COND_V_MSG(len == 0, ERR_CANT_CREATE, "Metal compiler created empty library");
	}

	// Strip the source from the binary. AIR versions before 2.4 (MSL 2.4) don't support
	// companion MetalLib, so metal-dsymutil copies verbatim and emits a warning:
	//   "architecture air64_v23 does not support a companion MetalLib; copying verbatim"
	if (device_profile->features.msl_version >= MSL_VERSION_24) {
		List<String> args{ "-sdk", sdk, "metal-dsymutil", "--remove-source", result_file->get_path_absolute() };
		String r_pipe;
		int exit_code;
		Error err = OS::get_singleton()->execute("/usr/bin/xcrun", args, &r_pipe, &exit_code, true);
		if (!r_pipe.is_empty()) {
			print_line(r_pipe);
		}
		if (err != OK) {
			ERR_PRINT(vformat("metal-dsymutil tool returned error code: %d", err));
		}

		if (exit_code != 0) {
			ERR_PRINT(vformat("metal-dsymutil Compiler exited with error code: %d", exit_code));
		}
		int len = result_file->get_length();
		ERR_FAIL_COND_V_MSG(len == 0, ERR_CANT_CREATE, "metal-dsymutil tool created empty library");
	}

	r_binary_data = result_file->get_buffer(result_file->get_length());

	return OK;
}

bool RenderingShaderContainerMetal::_use_rt_intersector() const {
	const String intersector_env = OS::get_singleton()->get_environment("GODOT_MTL_RT_INTERSECTOR");
	return intersector_env == "1" || (intersector_env != "0" && device_profile->gpu >= MetalDeviceProfile::GPU::Apple9);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"

static spv::ExecutionModel SHADER_STAGE_REMAP[RDD::SHADER_STAGE_MAX] = {
	spv::ExecutionModelVertex, // RDD::SHADER_STAGE_VERTEX
	spv::ExecutionModelFragment, // RDD::SHADER_STAGE_FRAGMENT
	spv::ExecutionModelTessellationControl, // RDD::SHADER_STAGE_TESSELATION_CONTROL
	spv::ExecutionModelTessellationEvaluation, // RDD::SHADER_STAGE_TESSELATION_EVALUATION
	spv::ExecutionModelGLCompute, // RDD::SHADER_STAGE_COMPUTE
	// The MSL lane cannot lower the ray-tracing pipeline stages below (see
	// docs/rt_metal_port/shader_strategy.md); they are mapped so that stage
	// bookkeeping never aliases them to ExecutionModelVertex (0).
	spv::ExecutionModelRayGenerationKHR, // RDD::SHADER_STAGE_RAYGEN
	spv::ExecutionModelAnyHitKHR, // RDD::SHADER_STAGE_ANY_HIT
	spv::ExecutionModelClosestHitKHR, // RDD::SHADER_STAGE_CLOSEST_HIT
	spv::ExecutionModelMissKHR, // RDD::SHADER_STAGE_MISS
	spv::ExecutionModelIntersectionKHR, // RDD::SHADER_STAGE_INTERSECTION
};

spv::ExecutionModel get_stage(uint32_t p_stages_mask, RDD::ShaderStage p_stage) {
	if (p_stages_mask & (1 << p_stage)) {
		return SHADER_STAGE_REMAP[p_stage];
	}
	return spv::ExecutionModel::ExecutionModelMax;
}

spv::ExecutionModel map_stage(RDD::ShaderStage p_stage) {
	return SHADER_STAGE_REMAP[p_stage];
}

MetalDeviceProfile::MinimumRequirements RenderingShaderContainerMetal::inspect_spirv(const ReflectShader &p_shader) {
	// Scan SPIR-V for OpImageTexelPointer to detect image atomic usage and determine
	// the minimum GPU family and MSL version required.
	// 32-bit atomics (R32ui/R32i) require Apple6+ and MSL 3.1+.
	// 64-bit atomics (Rg32ui/Rg32i) require Apple8+ and MSL 3.1+.

	MetalDeviceProfile::MinimumRequirements reqs;

	for (const ReflectShaderStage &stage : p_shader.shader_stages) {
		Span<uint32_t> spirv = stage.spirv();

		// SPIR-V header is 5 words (magic, version, generator, bound, schema).
		HashSet<uint32_t> atomic_image_ids;
		const uint32_t *words = spirv.ptr() + 5;
		while (words < spirv.end()) {
			const uint32_t instruction = *words;
			const uint16_t word_count = instruction >> 16;
			spv::Op opcode = static_cast<spv::Op>(instruction & 0xFFFF);
			if (opcode == spv::OpImageTexelPointer) {
				atomic_image_ids.insert(words[3]);
			}
			words += word_count;
		}

		if (atomic_image_ids.is_empty()) {
			continue;
		}

		// Look up image formats via spv-reflect bindings.
		const SpvReflectShaderModule &module = stage.module();
		uint32_t binding_count = 0;
		spvReflectEnumerateDescriptorBindings(&module, &binding_count, nullptr);

		LocalVector<SpvReflectDescriptorBinding *> bindings_heap;
		constexpr uint32_t MAX_STACK_BINDINGS = 256;

		SpvReflectDescriptorBinding **bindings;
		if (binding_count > MAX_STACK_BINDINGS) {
			bindings_heap.resize(binding_count);
			bindings = bindings_heap.ptr();
		} else {
			bindings = ALLOCA_ARRAY(SpvReflectDescriptorBinding *, binding_count);
		}
		spvReflectEnumerateDescriptorBindings(&module, &binding_count, bindings);

		for (uint32_t i = 0; i < binding_count; i++) {
			const SpvReflectDescriptorBinding &binding = *bindings[i];
			if (!atomic_image_ids.has(binding.spirv_id)) {
				continue;
			}

			switch (binding.image.image_format) {
				case SpvImageFormatR32ui:
				case SpvImageFormatR32i:
					if (reqs.gpu < MetalDeviceProfile::GPU::Apple6) {
						reqs.gpu = MetalDeviceProfile::GPU::Apple6;
					}
					reqs.msl_version = MAX(reqs.msl_version, MSL_VERSION_31);
					break;
				case SpvImageFormatRg32ui:
				case SpvImageFormatRg32i:
					if (reqs.gpu < MetalDeviceProfile::GPU::Apple8) {
						reqs.gpu = MetalDeviceProfile::GPU::Apple8;
					}
					reqs.msl_version = MAX(reqs.msl_version, MSL_VERSION_31);
					break;
				default:
					break;
			}
		}
	}

	return reqs;
}

bool RenderingShaderContainerMetal::_set_code_from_spirv(const ReflectShader &p_shader) {
	using namespace spirv_cross;
	using spirv_cross::CompilerMSL;
	using spirv_cross::Resource;

	const LocalVector<ReflectShaderStage> &p_spirv = p_shader.shader_stages;

	if (export_mode) {
		_initialize_toolchain_properties();

		// When baking shaders for export, check if the SPIR-V requires capabilities
		// that the target profile can't support natively. SPIRV-Cross would emulate
		// image atomics with auxiliary buffer bindings incompatible with Godot's binding
		// layout. Return an empty baked shader so the runtime recompiles for the actual device.
		MetalDeviceProfile::MinimumRequirements reqs = inspect_spirv(p_shader);
		MetalDeviceProfile::MinimumRequirements target = device_profile->get_minimum_requirements();
		if (reqs > target) {
			uint32_t req_maj, req_min, tgt_maj, tgt_min;
			parse_msl_version(reqs.msl_version, req_maj, req_min);
			parse_msl_version(target.msl_version, tgt_maj, tgt_min);
			WARN_PRINT(vformat("Shader '%s' requires Apple%d / MSL %d.%d but target is Apple%d / MSL %d.%d. Shader will be compiled at runtime on the device.",
					String(shader_name.ptr()),
					static_cast<uint32_t>(reqs.gpu) - 1000, req_maj, req_min,
					static_cast<uint32_t>(target.gpu) - 1000, tgt_maj, tgt_min));
			mtl_reflection_data.mark_invalid();
			return true;
		}
	}

	// initialize Metal-specific reflection data
	shaders.resize(p_spirv.size());
	mtl_shaders.resize(p_spirv.size());
	mtl_reflection_binding_set_uniforms_data.resize(reflection_binding_set_uniforms_data.size());

	mtl_reflection_data.set_needs_view_mask_buffer(reflection_data.has_multiview);
	mtl_reflection_data.profile = *device_profile;

	CompilerMSL::Options msl_options{};

	// Determine Metal language version.
	uint32_t msl_version = 0;
	{
		if (export_mode && compiler_props.is_valid()) {
			// Use the properties determined by the toolchain and minimum OS version.
			msl_version = compiler_props.metal_version;
			mtl_reflection_data.os_min_version = compiler_props.os_version_min_required;
		} else {
			msl_version = device_profile->features.msl_version;
			mtl_reflection_data.os_min_version = MinOsVersion();
		}
		uint32_t msl_ver_maj = 0;
		uint32_t msl_ver_min = 0;
		parse_msl_version(msl_version, msl_ver_maj, msl_ver_min);
		msl_options.set_msl_version(msl_ver_maj, msl_ver_min);
		mtl_reflection_data.msl_version = msl_version;
	}

	msl_options.platform = device_profile->platform == MetalDeviceProfile::Platform::macOS ? CompilerMSL::Options::macOS : CompilerMSL::Options::iOS;

	if (device_profile->platform == MetalDeviceProfile::Platform::iOS) {
		msl_options.ios_use_simdgroup_functions = device_profile->features.simdPermute;
		msl_options.ios_support_base_vertex_instance = true;
	}

	if (device_profile->features.use_argument_buffers) {
		msl_options.argument_buffers_tier = CompilerMSL::Options::ArgumentBuffersTier::Tier2;
		msl_options.argument_buffers = true;
		mtl_reflection_data.set_uses_argument_buffers(true);
	} else {
		msl_options.argument_buffers_tier = CompilerMSL::Options::ArgumentBuffersTier::Tier1;
		// Tier 1 argument buffers don't support writable textures, so we disable them completely.
		msl_options.argument_buffers = false;
		mtl_reflection_data.set_uses_argument_buffers(false);
	}
	msl_options.force_active_argument_buffer_resources = true;
	msl_options.pad_argument_buffer_resources = true;
	msl_options.texture_buffer_native = true; // Enable texture buffer support.
	msl_options.use_framebuffer_fetch_subpasses = false;
	msl_options.pad_fragment_output_components = true;
	msl_options.r32ui_alignment_constant_id = R32UI_ALIGNMENT_CONSTANT_ID;
	msl_options.agx_manual_cube_grad_fixup = true;
	if (reflection_data.has_multiview) {
		msl_options.multiview = true;
		msl_options.multiview_layered_rendering = true;
		msl_options.view_mask_buffer_index = VIEW_MASK_BUFFER_INDEX;
	}
	if (msl_version >= MSL_VERSION_32) {
		// All 3.2+ versions support device coherence, so we can disable texture fences.
		msl_options.readwrite_texture_fences = false;
	}

	CompilerGLSL::Options options{};
	options.vertex.flip_vert_y = true;
#if DEV_ENABLED
	options.emit_line_directives = true;
#endif

	// Assign MSL bindings for all the descriptor sets.
	typedef std::pair<MSLResourceBinding, uint32_t> MSLBindingInfo;
	LocalVector<MSLBindingInfo> spirv_bindings;
	MSLResourceBinding push_constant_resource_binding;
	// Sets that contain an unbounded (runtime-sized) array. SPIRV-Cross requires
	// those argument buffers to live in the device address space.
	uint32_t unbounded_arg_buffer_sets_mask = 0;
	{
		enum IndexType {
			Texture,
			Buffer,
			Sampler,
			Max,
		};

		uint32_t dset_count = p_shader.uniform_sets.size();
		uint32_t size = reflection_binding_set_uniforms_data.size();
		spirv_bindings.resize(size);

		uint32_t indices[IndexType::Max] = { 0 };
		auto next_index = [&indices](IndexType p_t, uint32_t p_stride) -> uint32_t {
			uint32_t v = indices[p_t];
			indices[p_t] += p_stride;
			return v;
		};

		uint32_t idx_dset = 0;
		MSLBindingInfo *iter = spirv_bindings.ptr();
		UniformData *found = mtl_reflection_binding_set_uniforms_data.ptrw();
		UniformData::IndexType shader_index_type = msl_options.argument_buffers ? UniformData::IndexType::ARG : UniformData::IndexType::SLOT;

		for (const ReflectDescriptorSet &dset : p_shader.uniform_sets) {
			bool set_has_unbounded = false;
			// Reset the index count for each descriptor set, as this is an index in to the argument table.
			uint32_t next_arg_buffer_index = 0;
			auto next_arg_index = [&next_arg_buffer_index](uint32_t p_stride) -> uint32_t {
				uint32_t v = next_arg_buffer_index;
				next_arg_buffer_index += p_stride;
				return v;
			};

			for (const ReflectUniform &uniform : dset) {
				const SpvReflectDescriptorBinding &binding = uniform.get_spv_reflect();

				found->active_stages = uniform.stages;

				// Bindings are iterated in ascending binding order, so any binding
				// after an unbounded one would sit past a runtime-sized region in
				// the argument buffer with no fixed offset.
				ERR_FAIL_COND_V_MSG(set_has_unbounded, false,
						vformat("Metal: an unbounded (runtime-sized) array must be the last binding of its descriptor set (set %d, binding %d follows one).", idx_dset, uniform.binding));

				RDC::UniformType type = RDC::UniformType(uniform.type);
				uint32_t binding_stride = 1; // If this is an array, stride will be the length of the array.
				if (uniform.unbounded) {
					// Runtime-sized (bindless) array: SPIRV-Cross lowers it as a
					// spvDescriptorArray over the trailing argument-buffer region,
					// which requires tier-2 argument buffers in the device address
					// space. The descriptor count is only known per uniform set.
					ERR_FAIL_COND_V_MSG(!msl_options.argument_buffers, false,
							vformat("Metal: unbounded (runtime-sized) arrays require tier-2 argument buffers (set %d, binding %d).", idx_dset, uniform.binding));
					ERR_FAIL_COND_V_MSG(type != RDC::UNIFORM_TYPE_TEXTURE, false,
							vformat("Metal: unbounded (runtime-sized) arrays are only supported for texture bindings (set %d, binding %d).", idx_dset, uniform.binding));
					set_has_unbounded = true;
					found->array_length = UniformData::UNBOUNDED_ARRAY_LENGTH;
				} else if (uniform.length > 1) {
					switch (type) {
						case RDC::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC:
						case RDC::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC:
						case RDC::UNIFORM_TYPE_UNIFORM_BUFFER:
						case RDC::UNIFORM_TYPE_STORAGE_BUFFER:
							// Buffers's length is its size, in bytes, so there is no stride.
							break;
						default: {
							binding_stride = uniform.length;
							found->array_length = uniform.length;
						} break;
					}
				}

				// Determine access type.
				switch (binding.descriptor_type) {
					case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
						if (!(binding.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE)) {
							if (!(binding.decoration_flags & SPV_REFLECT_DECORATION_NON_READABLE)) {
								found->access = MTL::BindingAccessReadWrite;
							} else {
								found->access = MTL::BindingAccessWriteOnly;
							}
						}
					} break;
					case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
					case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
						if (!(binding.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) && !(binding.block.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE)) {
							if (!(binding.decoration_flags & SPV_REFLECT_DECORATION_NON_READABLE) && !(binding.block.decoration_flags & SPV_REFLECT_DECORATION_NON_READABLE)) {
								found->access = MTL::BindingAccessReadWrite;
							} else {
								found->access = MTL::BindingAccessWriteOnly;
							}
						}
					} break;
					default:
						break;
				}

				switch (found->access) {
					case MTL::BindingAccessReadOnly:
						found->usage = MTL::ResourceUsageRead;
						break;
					case MTL::BindingAccessWriteOnly:
						found->usage = MTL::ResourceUsageWrite;
						break;
					case MTL::BindingAccessReadWrite:
						found->usage = MTL::ResourceUsageRead | MTL::ResourceUsageWrite;
						break;
				}

				iter->second = uniform.stages;
				MSLResourceBinding &rb = iter->first;
				rb.desc_set = idx_dset;
				rb.binding = uniform.binding;
				// A count of 0 keeps SPIRV-Cross on the runtime-sized descriptor
				// path (spvDescriptorArray) instead of a fixed-size array<T, N>.
				rb.count = uniform.unbounded ? 0 : binding_stride;
				if (uniform.unbounded) {
					ERR_FAIL_COND_V_MSG(idx_dset >= 32, false, "Metal: unbounded arrays are limited to descriptor sets 0-31.");
					unbounded_arg_buffer_sets_mask |= 1u << idx_dset;
				}

				switch (type) {
					case RDC::UNIFORM_TYPE_SAMPLER: {
						found->data_type = MTL::DataTypeSampler;
						found->get_indexes(UniformData::IndexType::SLOT).sampler = next_index(Sampler, binding_stride);
						found->get_indexes(UniformData::IndexType::ARG).sampler = next_arg_index(binding_stride);

						rb.basetype = SPIRType::BaseType::Sampler;

					} break;
					case RDC::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE:
					case RDC::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER: {
						found->data_type = MTL::DataTypeTexture;
						found->get_indexes(UniformData::IndexType::SLOT).texture = next_index(Texture, binding_stride);
						found->get_indexes(UniformData::IndexType::SLOT).sampler = next_index(Sampler, binding_stride);
						found->get_indexes(UniformData::IndexType::ARG).texture = next_arg_index(binding_stride);
						found->get_indexes(UniformData::IndexType::ARG).sampler = next_arg_index(binding_stride);
						rb.basetype = SPIRType::BaseType::SampledImage;
					} break;
					case RDC::UNIFORM_TYPE_TEXTURE:
					case RDC::UNIFORM_TYPE_IMAGE:
					case RDC::UNIFORM_TYPE_TEXTURE_BUFFER: {
						found->data_type = MTL::DataTypeTexture;
						found->get_indexes(UniformData::IndexType::SLOT).texture = next_index(Texture, binding_stride);
						found->get_indexes(UniformData::IndexType::ARG).texture = next_arg_index(binding_stride);
						rb.basetype = SPIRType::BaseType::Image;
					} break;
					case RDC::UNIFORM_TYPE_IMAGE_BUFFER:
						CRASH_NOW_MSG("Unimplemented!"); // TODO.
						break;
					case RDC::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC:
					case RDC::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC:
					case RDC::UNIFORM_TYPE_UNIFORM_BUFFER:
					case RDC::UNIFORM_TYPE_STORAGE_BUFFER: {
						found->data_type = MTL::DataTypePointer;
						found->get_indexes(UniformData::IndexType::SLOT).buffer = next_index(Buffer, binding_stride);
						found->get_indexes(UniformData::IndexType::ARG).buffer = next_arg_index(binding_stride);
						rb.basetype = SPIRType::BaseType::Void;
					} break;
					case RDC::UNIFORM_TYPE_INPUT_ATTACHMENT: {
						found->data_type = MTL::DataTypeTexture;
						found->get_indexes(UniformData::IndexType::SLOT).texture = next_index(Texture, binding_stride);
						found->get_indexes(UniformData::IndexType::ARG).texture = next_arg_index(binding_stride);
						rb.basetype = SPIRType::BaseType::Image;
					} break;
					case RDC::UNIFORM_TYPE_ACCELERATION_STRUCTURE: {
						found->data_type = MTL::DataTypeInstanceAccelerationStructure;
						found->get_indexes(UniformData::IndexType::SLOT).buffer = next_index(Buffer, binding_stride);
						found->get_indexes(UniformData::IndexType::ARG).buffer = next_arg_index(binding_stride);
						rb.basetype = SPIRType::BaseType::AccelerationStructure;
					} break;
					case RDC::UNIFORM_TYPE_MAX:
					default:
						CRASH_NOW_MSG("Unreachable");
				}

				// Specify the MSL resource bindings based on how the binding mode used by the shader.
				rb.msl_buffer = found->get_indexes(shader_index_type).buffer;
				rb.msl_texture = found->get_indexes(shader_index_type).texture;
				rb.msl_sampler = found->get_indexes(shader_index_type).sampler;

				if (found->data_type == MTL::DataTypeTexture) {
					const SpvReflectImageTraits &image = uniform.get_spv_reflect().image;

					switch (image.dim) {
						case SpvDim1D: {
							if (image.arrayed) {
								found->texture_type = MTL::TextureType1DArray;
							} else {
								found->texture_type = MTL::TextureType1D;
							}
						} break;
						case SpvDimSubpassData:
						case SpvDim2D: {
							if (image.arrayed && image.ms) {
								found->texture_type = MTL::TextureType2DMultisampleArray;
							} else if (image.arrayed) {
								found->texture_type = MTL::TextureType2DArray;
							} else if (image.ms) {
								found->texture_type = MTL::TextureType2DMultisample;
							} else {
								found->texture_type = MTL::TextureType2D;
							}
						} break;
						case SpvDim3D: {
							found->texture_type = MTL::TextureType3D;
						} break;
						case SpvDimCube: {
							if (image.arrayed) {
								found->texture_type = MTL::TextureTypeCubeArray;
							} else {
								found->texture_type = MTL::TextureTypeCube;
							}
						} break;
						case SpvDimRect: {
							// Ignored.
						} break;
						case SpvDimBuffer: {
							found->texture_type = MTL::TextureTypeTextureBuffer;
							// If this is used with atomics, we need to use a read-write texture.
							// 	scan_atomic_accesses();
							// 	if (atomic_spirv_ids.find(uniform.spirv_id) != atomic_spirv_ids.end()) {
							// 		rb.access = MTLBindingAccessReadWrite;
							// 		found->access = MTLBindingAccessReadWrite;
							// 	} else {
							// 		rb.access = MTLBindingAccessReadOnly;
							// 		found->access = MTLBindingAccessReadOnly;
							// 	}
						} break;
						case SpvDimTileImageDataEXT: {
							// Godot does not use this extension.
							// See: https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_shader_tile_image.html
						} break;
						case SpvDimMax: {
							// Add all enumerations to silence the compiler warning
							// and generate future warnings, should a new one be added.
						} break;
					}
				}

				iter++;
				found++;
			}
			idx_dset++;
		}

		if (reflection_data.push_constant_size > 0) {
			push_constant_resource_binding.desc_set = ResourceBindingPushConstantDescriptorSet;
			push_constant_resource_binding.basetype = SPIRType::BaseType::Void;
			if (msl_options.argument_buffers) {
				push_constant_resource_binding.msl_buffer = dset_count;
			} else {
				push_constant_resource_binding.msl_buffer = next_index(Buffer, 1);
			}
			mtl_reflection_data.push_constant_binding = push_constant_resource_binding.msl_buffer;
		}
	}

	for (uint32_t i = 0; i < p_spirv.size(); i++) {
		StageData &stage_data = mtl_shaders.write[i];
		const ReflectShaderStage &v = p_spirv[i];
		RDC::ShaderStage stage = v.shader_stage;
		Span<uint32_t> spirv = v.spirv();
		Parser parser(spirv.ptr(), spirv.size());
		try {
			parser.parse();
		} catch (CompilerError &e) {
			ERR_FAIL_V_MSG(false, "Failed to parse IR at stage " + String(RDC::SHADER_STAGE_NAMES[stage]) + ": " + e.what());
		}

		CompilerMSL compiler(std::move(parser.get_parsed_ir()));
		compiler.set_msl_options(msl_options);
		compiler.set_common_options(options);

		if (msl_options.argument_buffers && unbounded_arg_buffer_sets_mask != 0) {
			for (uint32_t set_index = 0; set_index < 32; set_index++) {
				if (unbounded_arg_buffer_sets_mask & (1u << set_index)) {
					compiler.set_argument_buffer_device_address_space(set_index, true);
				}
			}
		}

		spv::ExecutionModel execution_model = map_stage(stage);
		for (uint32_t jj = 0; jj < spirv_bindings.size(); jj++) {
			MSLResourceBinding &rb = spirv_bindings.ptr()[jj].first;
			rb.stage = execution_model;
			compiler.add_msl_resource_binding(rb);
		}

		if (push_constant_resource_binding.desc_set == ResourceBindingPushConstantDescriptorSet) {
			push_constant_resource_binding.stage = execution_model;
			compiler.add_msl_resource_binding(push_constant_resource_binding);
		}

		std::unordered_set<VariableID> active = compiler.get_active_interface_variables();
		ShaderResources resources = compiler.get_shader_resources();

		std::string source;
		try {
			source = compiler.compile();
		} catch (CompilerError &e) {
			ERR_FAIL_V_MSG(false, "Failed to compile stage " + String(RDC::SHADER_STAGE_NAMES[stage]) + ": " + e.what());
		}

		// Apple's Metal compiler can corrupt radiance values when the shadow-ray
		// intersection-query traversal is inlined into the path-tracing kernel.
		// This occurs with either one shared intersection query or separate primary
		// and shadow queries. Keep the shadow traversal behind a function boundary
		// by default; shader validation masks the issue by changing register
		// allocation. GODOT_MTL_RT_NOINLINE selects the boundary for controlled
		// experiments: "shadow" (default) marks only trace_shadow_blocked noinline,
		// "lights" restores the wider (slower) boundary around all direct lighting,
		// "none" fully inlines (fastest, known to corrupt).
		if (source.find("raytracing::intersection_query") != std::string::npos) {
			String noinline_mode = OS::get_singleton()->get_environment("GODOT_MTL_RT_NOINLINE");
			if (noinline_mode.is_empty()) {
				noinline_mode = "shadow";
			}
			static constexpr char always_inline_attr[] = "static inline __attribute__((always_inline))\n";
			static constexpr char noinline_attr[] = "static __attribute__((noinline))\n";
			const char *noinline_function = nullptr;
			if (noinline_mode == "lights") {
				noinline_function = "float3 lights_evaluate_direct_lighting";
			} else if (noinline_mode == "shadow") {
				noinline_function = "bool trace_shadow_blocked";
			}
			if (noinline_function != nullptr) {
				std::string inline_decl = std::string(always_inline_attr) + noinline_function;
				size_t position = source.find(inline_decl);
				if (position != std::string::npos) {
					source.replace(position, inline_decl.size(), std::string(noinline_attr) + noinline_function);
				}
			}
			// In shadow mode, additionally turn the hoisted by-reference shadow
			// query parameter into a function-local so the traversal state stays
			// register-allocated inside the noinline callee instead of paying
			// thread-memory traffic against main's frame.
			if (noinline_mode == "shadow" && OS::get_singleton()->get_environment("GODOT_MTL_RT_SHADOW_REF") != "1") {
				static constexpr char query_param[] = "& shadow_query)\n{";
				static constexpr char query_local[] =
						"& shadow_query_hoisted_unused)\n{\n"
						"    raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data> shadow_query;";
				size_t decl_pos = source.find("bool trace_shadow_blocked(");
				if (decl_pos != std::string::npos) {
					size_t param_pos = source.find(query_param, decl_pos);
					if (param_pos != std::string::npos) {
						source.replace(param_pos, sizeof(query_param) - 1, query_local);
					}
				}
			}

			// Native-intersector lane for ALL_OPAQUE pipelines; see
			// MetalRTShaderLowering::patch_scene_ray_query_to_intersector for the
			// full story. Default on for the Apple9+ hardware-RT tier, where Apple recommends the
			// intersector over intersection_query (measured 47.5 -> 64 fps at
			// 1080p/4spp/2bounce on M5); pre-Apple9 tiers keep the query path
			// until a benefit is measured there. GODOT_MTL_RT_INTERSECTOR=1
			// forces the lane on for A/B runs, =0 forces the query path.
			// The patch is all-or-nothing: if any anchor is missing it leaves
			// the source untouched and the query path keeps working.
			if (_use_rt_intersector()) {
				MetalRTShaderLowering::IntersectorPatchResult patch = MetalRTShaderLowering::patch_scene_ray_query_to_intersector(source);
				if (patch.applied()) {
					print_verbose("Metal RT: intersector fast path patched into " + String(shader_name.get_data()));
				} else {
					print_verbose(vformat("Metal RT: intersector fast path not applied to %s: [%s] %s", String(shader_name.get_data()),
							MetalRTShaderLowering::intersector_patch_status_name(patch.status), patch.detail));
				}
			} else if (OS::get_singleton()->get_environment("GODOT_MTL_RT_INTERSECTOR") == "0") {
				print_verbose("Metal RT: intersector fast path disabled by GODOT_MTL_RT_INTERSECTOR=0 for " + String(shader_name.get_data()));
			}
		}

		// Writes the generated MSL to GODOT_MTL_DUMP_MSL for offline inspection.
		if (String dump_dir = OS::get_singleton()->get_environment("GODOT_MTL_DUMP_MSL"); !dump_dir.is_empty()) {
			String name = String(shader_name.get_data()).validate_filename();
			String path = dump_dir.path_join(vformat("%s.%s.%d.metal", name, RDC::SHADER_STAGE_NAMES[stage], i));
			Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
			if (f.is_valid()) {
				f->store_string(String::utf8(source.c_str()));
			}
		}

		ERR_FAIL_COND_V_MSG(compiler.get_entry_points_and_stages().size() != 1, false, "Expected a single entry point and stage.");

		SmallVector<EntryPoint> entry_pts_stages = compiler.get_entry_points_and_stages();
		EntryPoint &entry_point_stage = entry_pts_stages.front();
		SPIREntryPoint &entry_point = compiler.get_entry_point(entry_point_stage.name, entry_point_stage.execution_model);

		for (auto ext : compiler.get_declared_extensions()) {
			if (ext == "SPV_KHR_non_semantic_info" || ext == "SPV_KHR_printf") {
				mtl_reflection_data.set_needs_debug_logging(true);
				break;
			}
		}

		if (!resources.stage_inputs.empty()) {
			for (Resource const &res : resources.stage_inputs) {
				uint32_t binding = compiler.get_automatic_msl_resource_binding(res.id);
				if (binding != (uint32_t)-1) {
					stage_data.vertex_input_binding_mask |= 1 << binding;
				}
			}
		}

		stage_data.is_position_invariant = compiler.is_position_invariant();
		stage_data.supports_fast_math = !entry_point.flags.get(spv::ExecutionModeSignedZeroInfNanPreserve);
		stage_data.hash = SHA256Digest(source.c_str(), source.length());
		stage_data.source_size = source.length();
		::Vector<uint8_t> binary_data;
		binary_data.resize(stage_data.source_size);
		memcpy(binary_data.ptrw(), source.c_str(), stage_data.source_size);

		if (export_mode) {
			if (compiler_props.is_valid()) {
				// Try to compile the Metal source code.
				::Vector<uint8_t> library_data;
				Error compile_err = compile_metal_source(source.c_str(), stage_data, library_data);
				if (compile_err == OK) {
					// If we successfully compiled to a `.metallib`, there are greater restrictions on target platforms,
					// so we must update the properties.
					stage_data.library_size = library_data.size();
					binary_data.resize(stage_data.source_size + stage_data.library_size);
					memcpy(binary_data.ptrw() + stage_data.source_size, library_data.ptr(), stage_data.library_size);
				}
			} else {
				WARN_PRINT_ONCE("Metal shader baking limited to SPIR-V: Unable to determine toolchain properties to compile .metallib");
			}
		}

		uint32_t binary_data_size = binary_data.size();
		Shader &shader = shaders.write[i];
		shader.shader_stage = stage;
		shader.code_decompressed_size = binary_data_size;
		shader.code_compressed_bytes.resize(binary_data_size);

		uint32_t compressed_size = 0;
		bool compressed = compress_code(binary_data.ptr(), binary_data_size, shader.code_compressed_bytes.ptrw(), &compressed_size, &shader.code_compression_flags);
		ERR_FAIL_COND_V_MSG(!compressed, false, vformat("Failed to compress native code to native for SPIR-V #%d.", i));

		shader.code_compressed_bytes.resize(compressed_size);
	}

	return true;
}

#pragma clang diagnostic pop

uint32_t RenderingShaderContainerMetal::_to_bytes_reflection_extra_data(uint8_t *p_bytes) const {
	if (p_bytes != nullptr) {
		*(HeaderData *)p_bytes = mtl_reflection_data;
	}
	return sizeof(HeaderData);
}

uint32_t RenderingShaderContainerMetal::_to_bytes_reflection_binding_uniform_extra_data(uint8_t *p_bytes, uint32_t p_index) const {
	if (is_invalid()) {
		return 0;
	}

	if (p_bytes != nullptr) {
		*(UniformData *)p_bytes = mtl_reflection_binding_set_uniforms_data[p_index];
	}
	return sizeof(UniformData);
}

uint32_t RenderingShaderContainerMetal::_to_bytes_shader_extra_data(uint8_t *p_bytes, uint32_t p_index) const {
	if (is_invalid()) {
		return 0;
	}

	if (p_bytes != nullptr) {
		*(StageData *)p_bytes = mtl_shaders[p_index];
	}
	return sizeof(StageData);
}

uint32_t RenderingShaderContainerMetal::_from_bytes_reflection_extra_data(const uint8_t *p_bytes) {
	mtl_reflection_data = *(HeaderData *)p_bytes;
	return sizeof(HeaderData);
}

uint32_t RenderingShaderContainerMetal::_from_bytes_reflection_binding_uniform_extra_data_start(const uint8_t *p_bytes) {
	mtl_reflection_binding_set_uniforms_data.resize(reflection_binding_set_uniforms_data.size());
	return 0;
}

uint32_t RenderingShaderContainerMetal::_from_bytes_reflection_binding_uniform_extra_data(const uint8_t *p_bytes, uint32_t p_index) {
	mtl_reflection_binding_set_uniforms_data.ptrw()[p_index] = *(UniformData *)p_bytes;
	return sizeof(UniformData);
}

uint32_t RenderingShaderContainerMetal::_from_bytes_shader_extra_data_start(const uint8_t *p_bytes) {
	mtl_shaders.resize(shaders.size());
	return 0;
}

uint32_t RenderingShaderContainerMetal::_from_bytes_shader_extra_data(const uint8_t *p_bytes, uint32_t p_index) {
	mtl_shaders.ptrw()[p_index] = *(StageData *)p_bytes;
	return sizeof(StageData);
}

RenderingShaderContainerMetal::MetalShaderReflection RenderingShaderContainerMetal::get_metal_shader_reflection() const {
	MetalShaderReflection res;

	uint32_t uniform_set_count = reflection_binding_set_uniforms_count.size();
	uint32_t start = 0;
	res.uniform_sets.resize(uniform_set_count);
	for (uint32_t i = 0; i < uniform_set_count; i++) {
		Vector<UniformData> &set = res.uniform_sets.ptrw()[i];
		uint32_t count = reflection_binding_set_uniforms_count.get(i);
		set.resize(count);
		memcpy(set.ptrw(), &mtl_reflection_binding_set_uniforms_data.ptr()[start], count * sizeof(UniformData));
		start += count;
	}

	return res;
}

bool RenderingShaderContainerMetal::is_rt_intersector_lane_compatible() const {
	const String name = String::utf8(shader_name.get_data());
	if (!name.begins_with("SceneRaytracingComputeShaderRD:") && !name.begins_with("RT_Metal_material_variant")) {
		return true;
	}

	const bool expected_intersector = _use_rt_intersector();
	Vector<uint8_t> decompressed_code;
	for (uint32_t shader_index = 0; shader_index < shaders.size(); shader_index++) {
		const RenderingShaderContainer::Shader &shader = shaders[shader_index];
		const StageData &shader_data = mtl_shaders[shader_index];
		if (shader.shader_stage != RDD::ShaderStage::SHADER_STAGE_COMPUTE || shader_data.source_size == 0) {
			continue;
		}

		if (shader.code_decompressed_size > 0) {
			decompressed_code.resize(shader.code_decompressed_size);
			if (!decompress_code(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size(), shader.code_compression_flags,
						decompressed_code.ptrw(), decompressed_code.size())) {
				return false;
			}
		} else {
			decompressed_code = shader.code_compressed_bytes;
		}
		if (shader_data.source_size > (uint32_t)decompressed_code.size()) {
			return false;
		}
		std::string source(reinterpret_cast<const char *>(decompressed_code.ptr()), shader_data.source_size);
		const bool compiled_intersector = source.find("godot_trace_material_intersector(") != std::string::npos;
		bool eligible = compiled_intersector;
		if (!eligible) {
			std::string patched_source = source;
			eligible = MetalRTShaderLowering::patch_scene_ray_query_to_intersector(patched_source).applied();
		}
		if (eligible && compiled_intersector != expected_intersector) {
			print_verbose(vformat("Metal RT: rejecting cached RT shader from the %s lane; the %s lane is active",
					compiled_intersector ? "intersector" : "query", expected_intersector ? "intersector" : "query"));
			return false;
		}
	}
	return true;
}

uint32_t RenderingShaderContainerMetal::_format() const {
	return 0x42424242;
}

uint32_t RenderingShaderContainerMetal::_format_version() const {
	return FORMAT_VERSION;
}

Ref<RenderingShaderContainer> RenderingShaderContainerFormatMetal::create_container() const {
	Ref<RenderingShaderContainerMetal> result;
	result.instantiate();
	result->set_export_mode(export_mode);
	result->set_device_profile(device_profile);
	return result;
}

RenderingDeviceCommons::ShaderLanguageVersion RenderingShaderContainerFormatMetal::get_shader_language_version() const {
	return SHADER_LANGUAGE_VULKAN_VERSION_1_1;
}

RenderingDeviceCommons::ShaderSpirvVersion RenderingShaderContainerFormatMetal::get_shader_spirv_version() const {
	return SHADER_SPIRV_VERSION_1_6;
}

RenderingShaderContainerFormatMetal::RenderingShaderContainerFormatMetal(const MetalDeviceProfile *p_device_profile, bool p_export) :
		export_mode(p_export), device_profile(p_device_profile) {
}

String MinOsVersion::to_compiler_os_version() const {
	if (version == UINT32_MAX) {
		return "";
	}

	uint32_t major = version / 10000;
	uint32_t minor = (version % 10000) / 100;
	return vformat("%d.%d", major, minor);
}

MinOsVersion::MinOsVersion(const String &p_version) {
	int pos = p_version.find_char('.');
	if (pos > 0) {
		version = (uint32_t)(p_version.substr(0, pos).to_int() * 10000 +
				p_version.substr(pos + 1).to_int() * 100);
	} else {
		version = (uint32_t)(p_version.to_int() * 10000);
	}

	if (version == 0) {
		version = UINT32_MAX;
	}
}
