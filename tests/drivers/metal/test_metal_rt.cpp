/**************************************************************************/
/*  test_metal_rt.cpp                                                     */
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

TEST_FORCE_LINK(test_metal_rt)

#ifdef METAL_ENABLED

#include "drivers/metal/metal_objects_shared.h"

namespace TestMetalRT {

TEST_CASE("[MetalRT] Acceleration structure metadata maps flags and scratch sizes") {
	MTL::AccelerationStructureSizes sizes = {};
	sizes.accelerationStructureSize = 4096;
	sizes.buildScratchBufferSize = 512;
	sizes.refitScratchBufferSize = 1024;

	BitField<RDD::AccelerationStructureFlagBits> flags = {};
	CHECK(MDAccelerationStructure::usage_from_flags(flags) == MTL::AccelerationStructureUsageNone);
	CHECK(MDAccelerationStructure::required_scratch_size(sizes, flags) == 512);

	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
	CHECK(MDAccelerationStructure::usage_from_flags(flags) == MTL::AccelerationStructureUsageRefit);
	CHECK(MDAccelerationStructure::required_scratch_size(sizes, flags) == 1024);

	flags.set_flag(RDD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT);
	CHECK(MDAccelerationStructure::usage_from_flags(flags) == (MTL::AccelerationStructureUsageRefit | MTL::AccelerationStructureUsagePreferFastBuild));

	sizes.buildScratchBufferSize = 2048;
	sizes.refitScratchBufferSize = 1024;
	CHECK(MDAccelerationStructure::required_scratch_size(sizes, flags) == 2048);

	BitField<RDD::AccelerationStructureFlagBits> deferred_flags = {};
	deferred_flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT);
	deferred_flags.set_flag(RDD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	deferred_flags.set_flag(RDD::ACCELERATION_STRUCTURE_LOW_MEMORY_BIT);
	CHECK(MDAccelerationStructure::usage_from_flags(deferred_flags) == MTL::AccelerationStructureUsageNone);
}

TEST_CASE("[MetalRT] Backend placeholders own descriptors and preserve metadata") {
	MTL::AccelerationStructureSizes sizes = {};
	sizes.accelerationStructureSize = 8192;
	sizes.buildScratchBufferSize = 768;
	sizes.refitScratchBufferSize = 1536;

	BitField<RDD::AccelerationStructureFlagBits> flags = {};
	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT);

	NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> blas_descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
	MTL::PrimitiveAccelerationStructureDescriptor *raw_blas_descriptor = blas_descriptor.get();
	MDAccelerationStructure blas(MDAccelerationStructure::Type::BLAS, blas_descriptor, sizes, flags);
	blas_descriptor.reset();

	CHECK(blas.type == MDAccelerationStructure::Type::BLAS);
	CHECK(blas.descriptor.get() == raw_blas_descriptor);
	CHECK_FALSE(blas.accel);
	CHECK(blas.acceleration_structure_size == 8192);
	CHECK(blas.scratch_size == 1536);
	CHECK(blas.flags.has_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT));
	CHECK(blas.flags.has_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT));
	CHECK(blas.max_instance_count == 0);

	NS::SharedPtr<MTL::InstanceAccelerationStructureDescriptor> tlas_descriptor = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
	MTL::InstanceAccelerationStructureDescriptor *raw_tlas_descriptor = tlas_descriptor.get();
	MDAccelerationStructure tlas(MDAccelerationStructure::Type::TLAS, tlas_descriptor, sizes, flags, 7);
	tlas_descriptor.reset();

	CHECK(tlas.type == MDAccelerationStructure::Type::TLAS);
	CHECK(tlas.descriptor.get() == raw_tlas_descriptor);
	CHECK_FALSE(tlas.accel);
	CHECK(tlas.max_instance_count == 7);

	MDPipeline *pipeline = new MDRaytracingPipeline;
	CHECK(pipeline->type == MDPipelineType::Raytracing);
	CHECK_FALSE(static_cast<MDRaytracingPipeline *>(pipeline)->state);
	delete pipeline;
}

} // namespace TestMetalRT

#endif // METAL_ENABLED
