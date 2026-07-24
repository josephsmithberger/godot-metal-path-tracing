/**************************************************************************/
/*  metal_rt_trace.cpp                                                    */
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

#include "metal_objects_shared.h"

// This deliberately small backend-owned kernel is the execution-path
// prototype. Even pixels trace the closest-hit path and odd pixels trace the
// miss path, producing an exact RGBA8 checkerboard that is safe to hash as a
// golden image on every supported GPU family.
static const char *TRACE_ONE_RAY_MSL = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>

using namespace metal;
using namespace metal::raytracing;

struct TraceConstants {
	uint width;
	uint height;
};

kernel void trace_one_ray(
		instance_acceleration_structure tlas [[buffer(0)]],
		device uchar4 *output [[buffer(1)]],
		constant TraceConstants &constants [[buffer(2)]],
		intersection_function_table<triangle_data, instancing> intersection_table [[buffer(3)]],
		uint2 tid [[thread_position_in_grid]]) {
	if (tid.x >= constants.width || tid.y >= constants.height) {
		return;
	}

	ray r;
	r.origin = float3(0.0f, -0.25f, -2.0f);
	r.min_distance = 0.001f;
	r.direction = ((tid.x + tid.y) & 1u) == 0u ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 0.0f, -1.0f);
	r.max_distance = 100.0f;

	intersector<triangle_data, instancing> isect;
	isect.assume_geometry_type(geometry_type::triangle);
	intersector<triangle_data, instancing>::result_type hit = isect.intersect(r, tlas, 0xffu, intersection_table);

	uint pixel_index = tid.y * constants.width + tid.x;
	output[pixel_index] = hit.type == intersection_type::triangle ?
			uchar4(232u, 168u, 32u, 255u) : uchar4(16u, 24u, 40u, 255u);
}
)MSL";

bool MDRaytracingPipeline::create_trace_one_ray(MTL::Device *p_device, String *r_error) {
	state.reset();
	intersection_function_table.reset();
	intersection_function_count = 0;
	if (r_error != nullptr) {
		r_error->clear();
	}

	if (p_device == nullptr || !p_device->supportsRaytracing()) {
		if (r_error != nullptr) {
			*r_error = "Metal ray tracing is not supported by this device.";
		}
		return false;
	}

	NS::Error *error = nullptr;
	NS::SharedPtr<MTL::CompileOptions> compile_options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
	compile_options->setLanguageVersion(MTL::LanguageVersion2_3);
	NS::SharedPtr<NS::String> source = NS::TransferPtr(NS::String::alloc()->init(TRACE_ONE_RAY_MSL, NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::Library> library = NS::TransferPtr(p_device->newLibrary(source.get(), compile_options.get(), &error));
	if (!library) {
		if (r_error != nullptr) {
			*r_error = error != nullptr ? error->localizedDescription()->utf8String() : "Unknown Metal library compilation error.";
		}
		return false;
	}

	NS::SharedPtr<NS::String> entry_name = NS::TransferPtr(NS::String::alloc()->init("trace_one_ray", NS::UTF8StringEncoding));
	NS::SharedPtr<MTL::Function> function = NS::TransferPtr(library->newFunction(entry_name.get()));
	if (!function) {
		if (r_error != nullptr) {
			*r_error = "Metal trace_one_ray entry point was not found.";
		}
		return false;
	}

	error = nullptr;
	state = NS::TransferPtr(p_device->newComputePipelineState(function.get(), &error));
	if (!state) {
		if (r_error != nullptr) {
			*r_error = error != nullptr ? error->localizedDescription()->utf8String() : "Unknown Metal compute-pipeline creation error.";
		}
		return false;
	}

	NS::SharedPtr<MTL::IntersectionFunctionTableDescriptor> table_descriptor = NS::TransferPtr(MTL::IntersectionFunctionTableDescriptor::alloc()->init());
	// Slot zero is always the system opaque-triangle function. The mapping reserves one
	// additional stable slot per procedural hit group; those slots are populated
	// by the compute lowering rather than Vulkan RT-stage functions.
	const uint32_t function_count = MAX(1u, uint32_t(intersection_functions.size()));
	table_descriptor->setFunctionCount(function_count);
	intersection_function_table = NS::TransferPtr(state->newIntersectionFunctionTable(table_descriptor.get()));
	if (!intersection_function_table) {
		state.reset();
		if (r_error != nullptr) {
			*r_error = "Failed to allocate the Metal intersection-function table.";
		}
		return false;
	}

	const MTL::IntersectionFunctionSignature signature = static_cast<MTL::IntersectionFunctionSignature>(
			MTL::IntersectionFunctionSignatureInstancing | MTL::IntersectionFunctionSignatureTriangleData);
	intersection_function_table->setOpaqueTriangleIntersectionFunction(signature, 0);
	intersection_function_count = function_count;
	return true;
}

bool MDRaytracingPipeline::encode_trace_one_ray(MTL::ComputeCommandEncoder *p_encoder, MTL::AccelerationStructure *p_tlas, MTL::Buffer *p_output_buffer, uint32_t p_width, uint32_t p_height) const {
	if (!is_valid() || p_encoder == nullptr || p_tlas == nullptr || p_output_buffer == nullptr || p_width == 0 || p_height == 0) {
		return false;
	}

	const uint64_t pixel_count = uint64_t(p_width) * uint64_t(p_height);
	if (pixel_count > UINT64_MAX / TRACE_PIXEL_SIZE_BYTES || p_output_buffer->length() < pixel_count * TRACE_PIXEL_SIZE_BYTES) {
		return false;
	}

	struct TraceConstants {
		uint32_t width;
		uint32_t height;
	} constants = { p_width, p_height };

	p_encoder->setComputePipelineState(state.get());
	p_encoder->setAccelerationStructure(p_tlas, TRACE_TLAS_BUFFER_INDEX);
	p_encoder->setBuffer(p_output_buffer, 0, TRACE_OUTPUT_BUFFER_INDEX);
	p_encoder->setBytes(&constants, sizeof(constants), TRACE_CONSTANTS_BUFFER_INDEX);
	p_encoder->setIntersectionFunctionTable(intersection_function_table.get(), TRACE_INTERSECTION_TABLE_BUFFER_INDEX);

	const uint32_t thread_width = MIN(p_width, uint32_t(state->threadExecutionWidth()));
	const uint32_t maximum_thread_height = uint32_t(state->maxTotalThreadsPerThreadgroup()) / thread_width;
	const uint32_t thread_height = MIN(p_height, MAX(1u, maximum_thread_height));
	p_encoder->dispatchThreads(MTL::Size(p_width, p_height, 1), MTL::Size(thread_width, thread_height, 1));
	return true;
}
