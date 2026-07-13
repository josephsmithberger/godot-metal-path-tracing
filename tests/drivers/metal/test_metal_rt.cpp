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
	CHECK(blas.build_scratch_size == 768);
	CHECK(blas.refit_scratch_size == 1536);
	CHECK(blas.scratch_size == 1536);
	CHECK_FALSE(blas.compacted_size_buffer);
	CHECK(blas.get_compacted_size() == 0);
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

TEST_CASE_PENDING("[MetalRT][GPU] Builds, queries, and refits a single-triangle BLAS") {
	NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
	NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
	if (!device || !device->supportsRaytracing()) {
		MESSAGE("SKIP_REASON=missing_metal_rt_feature");
		return;
	}

	struct TriangleVertex {
		float x;
		float y;
		float z;
	};
	const TriangleVertex vertices[] = {
		{ -1.0f, -1.0f, 0.0f },
		{ 1.0f, -1.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f },
	};
	NS::SharedPtr<MTL::Buffer> vertex_buffer = NS::TransferPtr(device->newBuffer(vertices, sizeof(vertices), MTL::ResourceStorageModeShared));
	REQUIRE(vertex_buffer);

	NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> geometry = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
	geometry->setVertexBuffer(vertex_buffer.get());
	geometry->setVertexBufferOffset(0);
	geometry->setVertexStride(sizeof(TriangleVertex));
	geometry->setTriangleCount(1);
	geometry->setOpaque(true);

	NS::Object *geometry_object = geometry.get();
	NS::SharedPtr<NS::Array> geometries = NS::TransferPtr(NS::Array::array(&geometry_object, 1)->retain());
	NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> descriptor = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
	descriptor->setGeometryDescriptors(geometries.get());

	BitField<RDD::AccelerationStructureFlagBits> flags = {};
	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
	flags.set_flag(RDD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT);
	descriptor->setUsage(MDAccelerationStructure::usage_from_flags(flags));

	MTL::AccelerationStructureSizes sizes = device->accelerationStructureSizes(descriptor.get());
	REQUIRE(sizes.accelerationStructureSize > 0);
	REQUIRE(sizes.buildScratchBufferSize > 0);

	NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
	REQUIRE(queue);

	for (uint32_t iteration = 0; iteration < 3; iteration++) {
		MDAccelerationStructure blas(MDAccelerationStructure::Type::BLAS, descriptor, sizes, flags);
		REQUIRE(blas.allocate(device.get()));
		REQUIRE(blas.accel);
		REQUIRE(blas.compacted_size_buffer);

		NS::SharedPtr<MTL::Buffer> scratch = NS::TransferPtr(device->newBuffer(blas.scratch_size, MTL::ResourceStorageModePrivate));
		REQUIRE(scratch);

		NS::SharedPtr<MTL::CommandBuffer> build_command = NS::RetainPtr(queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> build_encoder = NS::RetainPtr(build_command->accelerationStructureCommandEncoder());
		REQUIRE(build_encoder);
		blas.encode_build(build_encoder.get(), scratch.get());
		build_encoder->endEncoding();
		build_command->commit();
		build_command->waitUntilCompleted();

		REQUIRE(build_command->status() == MTL::CommandBufferStatusCompleted);
		CHECK(build_command->error() == nullptr);
		CHECK(blas.build_encoded);
		const uint64_t compacted_size = blas.get_compacted_size();
		CHECK(compacted_size > 0);
		CHECK(compacted_size <= blas.acceleration_structure_size);
		static_cast<TriangleVertex *>(vertex_buffer->contents())[2].y = 1.0f + (0.125f * (iteration + 1));

		NS::SharedPtr<MTL::CommandBuffer> refit_command = NS::RetainPtr(queue->commandBuffer());
		NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> refit_encoder = NS::RetainPtr(refit_command->accelerationStructureCommandEncoder());
		REQUIRE(refit_encoder);
		blas.encode_refit(refit_encoder.get(), scratch.get());
		refit_encoder->endEncoding();
		refit_command->commit();
		refit_command->waitUntilCompleted();

		REQUIRE(refit_command->status() == MTL::CommandBufferStatusCompleted);
		CHECK(refit_command->error() == nullptr);
		print_line(vformat("MetalRT C5 BLAS smoke: device=\"%s\" iteration=%d build_size=%d compacted_size=%d build_scratch_size=%d refit_scratch_size=%d", device->name()->utf8String(), iteration + 1, blas.acceleration_structure_size, compacted_size, blas.build_scratch_size, blas.refit_scratch_size));
	}
}

} // namespace TestMetalRT

#endif // METAL_ENABLED
